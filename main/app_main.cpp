#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "config.h"
#include "ddc.h"
#include "edid.h"
#include "mccs.h"
#include "mdns.h"
#include "matter.h"
#include "monitor_db.h"
#include "webserver.h"
}

static const char *TAG = "app_main";
static constexpr uint32_t POST_COMMISSION_TASK_STACK_SIZE = 8192;
static constexpr TickType_t INPUT_PROBE_SETTLE_DELAY = pdMS_TO_TICKS(1200);
static constexpr TickType_t LG_INPUT_PROBE_SETTLE_DELAY = pdMS_TO_TICKS(5000);
static constexpr TickType_t WEB_MDNS_RETRY_DELAY = pdMS_TO_TICKS(1000);
static constexpr size_t MAX_PROBE_INPUT_VALUES = 24;
static constexpr uint8_t WEB_MDNS_MAX_RETRIES = 5;
static constexpr uint8_t LG_INPUT_FINGERPRINT_VCP = 0xF8;
static constexpr char kWebMdnsHostname[] = "display-switcher";
static constexpr char kWebMdnsInstance[] = "Display Switcher";
static constexpr char kWebMdnsServiceType[] = "_http";
static constexpr char kWebMdnsServiceProto[] = "_tcp";
static constexpr uint16_t kWebServerPort = 80;
static constexpr char kWifiStaNetifKey[] = "WIFI_STA_DEF";
static constexpr char kEthNetifKey[] = "ETH_DEF";

typedef struct {
    ddc_bus_t ddc;
    display_config_t config;
    cached_profile_t cache;
    monitor_profile_t profile;
    matter_runtime_t matter;
    webserver_context_t web;
    bool monitor_available;
    bool post_commission_started;
    bool webserver_started;
    bool web_mdns_retry_pending;
    bool last_requested_input_valid;
    uint8_t last_requested_input_value;
    uint8_t last_known_mode;
} app_context_t;

static void sync_runtime_state(app_context_t *app);
static void schedule_web_mdns_retry(app_context_t *app);

static esp_netif_t *get_primary_netif(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(kWifiStaNetifKey);
    if (netif != nullptr) {
        return netif;
    }
    return esp_netif_get_handle_from_ifkey(kEthNetifKey);
}

static bool web_mdns_uses_local_hostname(void)
{
    char hostname[MDNS_NAME_BUF_LEN] = {};
    return mdns_hostname_get(hostname) == ESP_OK && std::strcmp(hostname, kWebMdnsHostname) == 0;
}

static bool get_web_mdns_address(mdns_ip_addr_t *address)
{
    esp_netif_t *netif = get_primary_netif();
    if (netif == nullptr) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    std::memset(address, 0, sizeof(*address));
    address->addr.type = ESP_IPADDR_TYPE_V4;
    address->addr.u_addr.ip4 = ip_info.ip;
    return true;
}

static void disable_web_mdns_alias(void)
{
    const char *service_host = web_mdns_uses_local_hostname() ? nullptr : kWebMdnsHostname;
    if (mdns_service_exists(kWebMdnsServiceType, kWebMdnsServiceProto, service_host)) {
        esp_err_t remove_err = mdns_service_remove_for_host(NULL, kWebMdnsServiceType, kWebMdnsServiceProto,
                                                            service_host);
        if (remove_err != ESP_OK && remove_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "failed to remove web mdns service: %s", esp_err_to_name(remove_err));
        }
    }

    if (!web_mdns_uses_local_hostname() && mdns_hostname_exists(kWebMdnsHostname)) {
        esp_err_t host_err = mdns_delegate_hostname_remove(kWebMdnsHostname);
        if (host_err != ESP_OK && host_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "failed to remove web mdns hostname: %s", esp_err_to_name(host_err));
        }
    }
}

static esp_err_t refresh_web_mdns_alias(app_context_t *app)
{
    if (!app->webserver_started) {
        return ESP_OK;
    }

    mdns_ip_addr_t address = {};
    if (!get_web_mdns_address(&address)) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool use_local_hostname = web_mdns_uses_local_hostname();
    if (!use_local_hostname) {
        esp_err_t host_err = mdns_hostname_exists(kWebMdnsHostname)
                                 ? mdns_delegate_hostname_set_address(kWebMdnsHostname, &address)
                                 : mdns_delegate_hostname_add(kWebMdnsHostname, &address);
        ESP_RETURN_ON_ERROR(host_err, TAG, "web mdns hostname update failed");
    }

    const char *service_host = use_local_hostname ? nullptr : kWebMdnsHostname;
    if (mdns_service_exists(kWebMdnsServiceType, kWebMdnsServiceProto, service_host)) {
        esp_err_t remove_err = mdns_service_remove_for_host(NULL, kWebMdnsServiceType, kWebMdnsServiceProto,
                                                            service_host);
        if (remove_err != ESP_OK && remove_err != ESP_ERR_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(remove_err, TAG, "web mdns service remove failed");
        }
    }

    mdns_txt_item_t txt[] = {{"path", "/"}};
    ESP_RETURN_ON_ERROR(mdns_service_add_for_host(kWebMdnsInstance, kWebMdnsServiceType, kWebMdnsServiceProto,
                                                  service_host, kWebServerPort, txt, 1),
                        TAG, "web mdns service add failed");

    ESP_LOGI(TAG, "web UI advertised via http://%s.local/", kWebMdnsHostname);
    return ESP_OK;
}

static void network_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;

    app_context_t *app = static_cast<app_context_t *>(arg);
    if (!app->post_commission_started || !app->webserver_started) {
        return;
    }

    esp_err_t err = refresh_web_mdns_alias(app);
    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG) {
        ESP_LOGI(TAG, "web mdns waiting for Matter mdns initialization to finish");
        schedule_web_mdns_retry(app);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "web mdns update failed: %s", esp_err_to_name(err));
    }
}

static void web_mdns_retry_task(void *arg)
{
    app_context_t *app = static_cast<app_context_t *>(arg);

    for (uint8_t attempt = 0; attempt < WEB_MDNS_MAX_RETRIES; ++attempt) {
        vTaskDelay(WEB_MDNS_RETRY_DELAY);

        esp_err_t err = refresh_web_mdns_alias(app);
        if (err == ESP_OK) {
            app->web_mdns_retry_pending = false;
            vTaskDelete(NULL);
            return;
        }

        if (err != ESP_ERR_INVALID_STATE && err != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "web mdns retry failed: %s", esp_err_to_name(err));
            app->web_mdns_retry_pending = false;
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGW(TAG, "web mdns publish did not become ready after retries");
    app->web_mdns_retry_pending = false;
    vTaskDelete(NULL);
}

static void schedule_web_mdns_retry(app_context_t *app)
{
    if (app->web_mdns_retry_pending) {
        return;
    }

    app->web_mdns_retry_pending = true;
    BaseType_t task_ok = xTaskCreate(web_mdns_retry_task, "web_mdns_retry", 4096, app, 5, NULL);
    if (task_ok != pdPASS) {
        app->web_mdns_retry_pending = false;
        ESP_LOGW(TAG, "failed to create web mdns retry task");
    }
}

static uint8_t matter_level_to_ddc_value(uint8_t matter_level)
{
    if (matter_level >= 254) {
        return 100;
    }
    return static_cast<uint8_t>((static_cast<uint16_t>(matter_level) * 100U + 127U) / 254U);
}

static void copy_inputs_from_slots(display_config_t *config, const input_slot_t *slots)
{
    for (size_t i = 0; i < INPUT_SLOT_COUNT; ++i) {
        config->inputs[i] = slots[i];
    }
}

static bool find_input_slot_for_endpoint(const matter_runtime_t *runtime, uint16_t endpoint_id, uint8_t *slot)
{
    for (uint8_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        if (runtime->input_endpoint_ids[index] == endpoint_id) {
            *slot = index;
            return true;
        }
    }
    return false;
}

static bool has_input_value(const uint8_t *values, size_t count, uint16_t value)
{
    for (size_t index = 0; index < count; ++index) {
        if (values[index] == value) {
            return true;
        }
    }
    return false;
}

static bool monitor_uses_lg_inputs(const app_context_t *app)
{
    return mccs_monitor_uses_lg_inputs(app->config.monitor_name, app->config.pnp_id);
}

static TickType_t input_probe_settle_delay(const app_context_t *app)
{
    return monitor_uses_lg_inputs(app) ? LG_INPUT_PROBE_SETTLE_DELAY : INPUT_PROBE_SETTLE_DELAY;
}

static esp_err_t read_current_input_value(app_context_t *app, ddc_vcp_value_t *value)
{
    esp_err_t err = ddc_get_input_source(&app->ddc, value);
    if (err == ESP_OK && value->present) {
        return ESP_OK;
    }

    if (!monitor_uses_lg_inputs(app)) {
        return err;
    }

    ddc_vcp_value_t fingerprint = {};
    esp_err_t fingerprint_err = ddc_get_vcp(&app->ddc, LG_INPUT_FINGERPRINT_VCP, &fingerprint);
    if (fingerprint_err != ESP_OK || !fingerprint.present) {
        return err;
    }

    uint8_t fingerprint_value = static_cast<uint8_t>(fingerprint.current_value & 0xff);
    uint8_t matched_mode = 0;
    uint8_t matched_value = 0;
    size_t matched_count = 0;

    for (uint8_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        uint8_t candidate = app->config.inputs[index].value;
        if (!mccs_input_matches_lg_fingerprint(candidate, fingerprint_value)) {
            continue;
        }

        matched_mode = index;
        matched_value = candidate;
        ++matched_count;
    }

    if (matched_count == 1) {
        std::memset(value, 0, sizeof(*value));
        value->present = true;
        value->current_value = matched_value;
        value->maximum_value = 0xff;
        app->last_requested_input_valid = true;
        app->last_requested_input_value = matched_value;
        app->last_known_mode = matched_mode;
        ESP_LOGI(TAG, "LG input fingerprint 0x%02X resolved uniquely to 0x%02X", fingerprint_value, matched_value);
        return ESP_OK;
    }

    if (app->last_requested_input_valid &&
        mccs_input_matches_lg_fingerprint(app->last_requested_input_value, fingerprint_value)) {
        std::memset(value, 0, sizeof(*value));
        value->present = true;
        value->current_value = app->last_requested_input_value;
        value->maximum_value = 0xff;
        ESP_LOGI(TAG, "LG input fingerprint 0x%02X fell back to last requested 0x%02X", fingerprint_value,
                 app->last_requested_input_value);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "LG input fingerprint 0x%02X was ambiguous across %u configured inputs", fingerprint_value,
             (unsigned int)matched_count);
    return err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED;
}

static void apply_discovered_inputs(display_config_t *config, const uint8_t *values, size_t count)
{
    input_slot_t slots[INPUT_SLOT_COUNT] = {};
    input_slot_t defaults[INPUT_SLOT_COUNT] = {};
    size_t out = 0;

    for (size_t index = 0; index < count && out < INPUT_SLOT_COUNT; ++index) {
        slots[out].value = values[index];
        slots[out].enabled = true;
        std::strncpy(slots[out].name, mccs_input_label(values[index]), sizeof(slots[out].name) - 1);
        ++out;
    }

    mccs_fill_default_inputs_for_display(defaults, INPUT_SLOT_COUNT, config->monitor_name, config->pnp_id);
    for (size_t index = 0; index < INPUT_SLOT_COUNT && out < INPUT_SLOT_COUNT; ++index) {
        if (has_input_value(values, count, defaults[index].value)) {
            continue;
        }
        slots[out++] = defaults[index];
    }

    copy_inputs_from_slots(config, slots);
}

static bool try_find_mode_for_input(const display_config_t *config, uint16_t value, uint8_t *mode)
{
    for (uint8_t i = 0; i < INPUT_SLOT_COUNT; ++i) {
        if (config->inputs[i].value == value) {
            *mode = i;
            return true;
        }
    }
    return false;
}

static void apply_safe_defaults(display_config_t *config)
{
    std::memset(config, 0, sizeof(*config));
    std::strncpy(config->pnp_id, "UNK", sizeof(config->pnp_id) - 1);
    std::strncpy(config->monitor_name, "Display", sizeof(config->monitor_name) - 1);
    config->brightness_vcp = 0x10;
    config->contrast_vcp = 0x12;
    mccs_fill_default_inputs_for_display(config->inputs, INPUT_SLOT_COUNT, config->monitor_name, config->pnp_id);
}

static void preload_saved_config(display_config_t *config)
{
    display_config_t stored_user = {};
    bool user_found = false;

    if (config_load_user(&stored_user, &user_found) != ESP_OK || !user_found || !stored_user.user_override) {
        return;
    }

    *config = stored_user;
}

static void log_startup_i2c_probe(app_context_t *app)
{
    static const uint8_t kExpectedEdidHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

    esp_err_t edid_probe_err = i2c_master_probe(app->ddc.bus, DDC_EDID_ADDRESS, 100);
    esp_err_t ddc_ci_probe_err = i2c_master_probe(app->ddc.bus, DDC_CI_ADDRESS, 100);
    ESP_LOGI(TAG, "startup I2C probe: EDID(0x50)=%s DDC/CI(0x37)=%s", esp_err_to_name(edid_probe_err),
             esp_err_to_name(ddc_ci_probe_err));

    if (edid_probe_err != ESP_OK) {
        return;
    }

    uint8_t header[sizeof(kExpectedEdidHeader)] = {0};
    esp_err_t header_read_err = ddc_read_edid(&app->ddc, header, sizeof(header));
    if (header_read_err != ESP_OK) {
        ESP_LOGW(TAG, "startup EDID header read failed: %s", esp_err_to_name(header_read_err));
        return;
    }

    bool header_valid = std::memcmp(header, kExpectedEdidHeader, sizeof(header)) == 0;
    ESP_LOGI(TAG,
             "startup EDID header: %02x %02x %02x %02x %02x %02x %02x %02x (%s)",
             header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7],
             header_valid ? "valid" : "unexpected");
}

static esp_err_t detect_monitor(app_context_t *app, bool force_remote_refresh)
{
    uint8_t edid[128] = {0};
    edid_info_t info = {};
    input_slot_t slots[INPUT_SLOT_COUNT] = {};
    display_config_t stored_user = {};
    bool user_found = false;

    app->monitor_available = false;
    std::memset(&app->cache, 0, sizeof(app->cache));
    std::memset(&app->profile, 0, sizeof(app->profile));
    ESP_RETURN_ON_ERROR(ddc_read_edid(&app->ddc, edid, sizeof(edid)), TAG, "edid read failed");
    ESP_RETURN_ON_ERROR(edid_parse(edid, sizeof(edid), &info), TAG, "edid parse failed");
    app->monitor_available = true;

    apply_safe_defaults(&app->config);
    std::strncpy(app->config.pnp_id, info.pnp_id, sizeof(app->config.pnp_id) - 1);
    if (info.has_name) {
        std::strncpy(app->config.monitor_name, info.monitor_name, sizeof(app->config.monitor_name) - 1);
    }

    ESP_RETURN_ON_ERROR(config_load_user(&stored_user, &user_found), TAG, "load user config failed");
    if (user_found && stored_user.user_override && !force_remote_refresh) {
        app->config = stored_user;
        std::strncpy(app->config.pnp_id, info.pnp_id, sizeof(app->config.pnp_id) - 1);
        if (info.has_name) {
            std::strncpy(app->config.monitor_name, info.monitor_name, sizeof(app->config.monitor_name) - 1);
        }
        app->config.profile_cached = false;
        app->config.db_match = false;
        ESP_LOGI(TAG, "using user override config");
        return ESP_OK;
    }

    if (force_remote_refresh) {
        ESP_LOGI(TAG, "remote monitor database disabled; re-reading on-bus capabilities");
    }

    esp_err_t clear_cache_err = config_clear_cached_profile();
    if (clear_cache_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to clear cached monitor profile: %s", esp_err_to_name(clear_cache_err));
    }

    char caps[PROFILE_CAPS_MAX_LEN] = {0};
    if (ddc_query_capabilities(&app->ddc, caps, sizeof(caps)) == ESP_OK) {
        ESP_LOGI(TAG, "monitor capabilities: %s", caps);
        size_t count = monitor_db_parse_input_values(caps, slots, INPUT_SLOT_COUNT);
        if (count > 0) {
            if (count < INPUT_SLOT_COUNT) {
                mccs_fill_default_inputs_for_display(&slots[count], INPUT_SLOT_COUNT - count, app->config.monitor_name,
                                                    app->config.pnp_id);
            }
            copy_inputs_from_slots(&app->config, slots);
            app->config.db_match = false;
            app->config.profile_cached = false;
            ESP_LOGI(TAG, "using direct DDC capabilities");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "capabilities string did not advertise usable input values for VCP 0x60");
    }

    mccs_fill_default_inputs_for_display(app->config.inputs, INPUT_SLOT_COUNT, app->config.monitor_name,
                                         app->config.pnp_id);
    app->config.db_match = false;
    app->config.profile_cached = false;
    ESP_LOGI(TAG, "using MCCS defaults");
    return ESP_OK;
}

static esp_err_t set_input_value(app_context_t *app, uint8_t input_value)
{
    esp_err_t err = ddc_set_input_source(&app->ddc, input_value);
    if (err == ESP_OK) {
        app->last_requested_input_valid = true;
        app->last_requested_input_value = input_value;

        uint8_t mode = 0;
        if (try_find_mode_for_input(&app->config, input_value, &mode)) {
            app->last_known_mode = mode;
        }
    }
    return err;
}

static esp_err_t get_level_value(app_context_t *app, bool contrast, uint8_t vcp, ddc_vcp_value_t *value)
{
    if (!app->monitor_available) {
        return ESP_ERR_INVALID_STATE;
    }
    return ddc_get_vcp(&app->ddc, vcp, value);
}

static esp_err_t get_input_source_state(app_context_t *app, web_input_source_state_t *state)
{
    std::memset(state, 0, sizeof(*state));
    state->matched_slot = -1;

    if (!app->monitor_available) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t standard_err = ddc_get_input_source_standard(&app->ddc, &state->standard);
    if (standard_err != ESP_OK) {
        std::memset(&state->standard, 0, sizeof(state->standard));
    }

    esp_err_t alternate_err = ddc_get_input_source_alternate(&app->ddc, &state->alternate);
    if (alternate_err != ESP_OK) {
        std::memset(&state->alternate, 0, sizeof(state->alternate));
    }

    esp_err_t resolved_err = read_current_input_value(app, &state->resolved);
    if (resolved_err != ESP_OK) {
        std::memset(&state->resolved, 0, sizeof(state->resolved));
    } else if (state->resolved.present) {
        uint8_t matched_mode = 0;
        if (try_find_mode_for_input(&app->config, state->resolved.current_value, &matched_mode)) {
            state->matched_slot = matched_mode;
            std::strncpy(state->matched_name, app->config.inputs[matched_mode].name, sizeof(state->matched_name) - 1);
        }
    }

    if (state->standard.present || state->alternate.present || state->resolved.present) {
        return ESP_OK;
    }
    return resolved_err != ESP_OK ? resolved_err : ESP_FAIL;
}

static esp_err_t matter_level_write(uint16_t endpoint_id, uint8_t level, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    if (!app->monitor_available) {
        ESP_LOGW(TAG, "ignoring Matter level write while monitor is unavailable");
        return ESP_OK;
    }

    uint8_t ddc_value = matter_level_to_ddc_value(level);
    uint8_t vcp = (endpoint_id == app->matter.contrast_endpoint_id) ? app->config.contrast_vcp : app->config.brightness_vcp;
    ESP_LOGI(TAG, "Matter level write endpoint=%u level=%u mapped_ddc=%u vcp=0x%02X", endpoint_id, level, ddc_value, vcp);
    return ddc_set_vcp(&app->ddc, vcp, ddc_value);
}

static esp_err_t matter_input_write(uint16_t endpoint_id, bool on, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    uint8_t slot = 0;

    if (!on) {
        return ESP_OK;
    }
    if (!find_input_slot_for_endpoint(&app->matter, endpoint_id, &slot)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (slot >= INPUT_SLOT_COUNT || !app->config.inputs[slot].enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app->monitor_available) {
        ESP_LOGW(TAG, "ignoring Matter input write while monitor is unavailable");
        return ESP_OK;
    }
    return set_input_value(app, app->config.inputs[slot].value);
}

static esp_err_t apply_config_cb(const display_config_t *config, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    app->config = *config;
    app->config.user_override = true;
    app->config.profile_cached = false;
    app->config.db_match = false;
    ESP_RETURN_ON_ERROR(matter_sync_input_endpoints(&app->config), TAG, "input endpoint sync failed");
    ESP_RETURN_ON_ERROR(config_save_user(&app->config), TAG, "save user config failed");
    return ESP_OK;
}

static esp_err_t test_input_cb(uint8_t value, void *ctx)
{
    return set_input_value(static_cast<app_context_t *>(ctx), value);
}

static esp_err_t probe_inputs_cb(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    uint8_t candidates[MAX_PROBE_INPUT_VALUES] = {};
    uint8_t discovered[MAX_PROBE_INPUT_VALUES] = {};
    ddc_vcp_value_t original = {};
    size_t candidate_count = 0;
    size_t discovered_count = 0;
    TickType_t settle_delay = input_probe_settle_delay(app);
    uint8_t restore_value = 0;
    bool restore_value_valid = false;

    if (!app->monitor_available) {
        return ESP_ERR_INVALID_STATE;
    }

    if (read_current_input_value(app, &original) == ESP_OK && original.present) {
        restore_value = static_cast<uint8_t>(original.current_value & 0xff);
        restore_value_valid = true;
        discovered[discovered_count++] = restore_value;
    } else if (app->last_requested_input_valid) {
        restore_value = app->last_requested_input_value;
        restore_value_valid = true;
        discovered[discovered_count++] = restore_value;
        ESP_LOGW(TAG, "using last requested input 0x%02X as probe restore target", restore_value);
    } else {
        restore_value = app->config.inputs[app->last_known_mode].value;
        restore_value_valid = true;
        discovered[discovered_count++] = restore_value;
        ESP_LOGW(TAG, "live input readback unavailable; falling back to slot %u value 0x%02X for restore",
                 (unsigned int)app->last_known_mode, restore_value);
    }

    ESP_RETURN_ON_FALSE(restore_value_valid, ESP_ERR_NOT_SUPPORTED, TAG, "no restore input available");
    candidate_count = mccs_get_probe_input_values(app->config.monitor_name, app->config.pnp_id, candidates,
                                                  sizeof(candidates) / sizeof(candidates[0]));
    ESP_RETURN_ON_FALSE(candidate_count > 0, ESP_ERR_NOT_FOUND, TAG, "no probe candidates");

    ESP_LOGI(TAG, "probing %u known input values using input readback", (unsigned int)candidate_count);
    for (size_t index = 0; index < candidate_count; ++index) {
        uint8_t requested = candidates[index];
        ddc_vcp_value_t resolved = {};

        if (set_input_value(app, requested) != ESP_OK) {
            ESP_LOGW(TAG, "input probe write failed for 0x%02X", requested);
            continue;
        }

        vTaskDelay(settle_delay);
        if (read_current_input_value(app, &resolved) != ESP_OK || !resolved.present) {
            if (!monitor_uses_lg_inputs(app)) {
                ESP_LOGW(TAG, "input probe readback failed after 0x%02X", requested);
                continue;
            }

            resolved.present = true;
            resolved.current_value = requested;
            resolved.maximum_value = 0xff;
            ESP_LOGW(TAG, "LG input probe falling back to requested value 0x%02X after unreadable readback", requested);
        }

        uint8_t canonical = static_cast<uint8_t>(resolved.current_value & 0xff);
        ESP_LOGI(TAG, "input probe requested 0x%02X -> readback 0x%02X", requested, canonical);
        if (!has_input_value(discovered, discovered_count, canonical) && discovered_count < MAX_PROBE_INPUT_VALUES) {
            discovered[discovered_count++] = canonical;
        }
    }

    if (set_input_value(app, restore_value) == ESP_OK) {
        vTaskDelay(settle_delay);
    }

    apply_discovered_inputs(&app->config, discovered, discovered_count);
    app->config.db_match = false;
    app->config.profile_cached = false;
    ESP_RETURN_ON_ERROR(matter_sync_input_endpoints(&app->config), TAG, "input endpoint sync failed");
    sync_runtime_state(app);
    ESP_LOGI(TAG, "input probe discovered %u unique input values", (unsigned int)discovered_count);
    return ESP_OK;
}

static esp_err_t detect_cb(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    ESP_RETURN_ON_ERROR(detect_monitor(app, false), TAG, "detect failed");
    return matter_sync_input_endpoints(&app->config);
}

static esp_err_t refresh_db_cb(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    ESP_RETURN_ON_ERROR(detect_monitor(app, true), TAG, "db refresh detect failed");
    return matter_sync_input_endpoints(&app->config);
}

static esp_err_t get_level_cb(bool contrast, uint8_t vcp, ddc_vcp_value_t *value, void *ctx)
{
    return get_level_value(static_cast<app_context_t *>(ctx), contrast, vcp, value);
}

static esp_err_t set_level_cb(bool contrast, uint8_t vcp, uint8_t value, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    if (!app->monitor_available) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ddc_set_vcp(&app->ddc, vcp, value), TAG, "set vcp failed");

    uint16_t endpoint_id = contrast ? app->matter.contrast_endpoint_id : app->matter.brightness_endpoint_id;
    ESP_RETURN_ON_ERROR(matter_update_level(endpoint_id, value), TAG, "matter level sync failed");
    return ESP_OK;
}

static esp_err_t open_commissioning_window_cb(void *ctx)
{
    (void)ctx;
    return matter_open_basic_commissioning_window();
}

static esp_err_t get_input_source_state_cb(web_input_source_state_t *state, void *ctx)
{
    return get_input_source_state(static_cast<app_context_t *>(ctx), state);
}

/* Called once when the device first gets an IP address (post-commissioning).
 * If we previously fell through to DDC-caps or MCCS defaults, try the remote
 * ddccontrol-db fetch now that network is available.  Runs in a dedicated task
 * so I2C is not driven from the Matter event-loop task. */
static void sync_runtime_state(app_context_t *app)
{
    if (app->monitor_available) {
        ddc_vcp_value_t brightness = {};
        if (ddc_get_vcp(&app->ddc, app->config.brightness_vcp, &brightness) == ESP_OK && brightness.present) {
            matter_update_level(app->matter.brightness_endpoint_id, static_cast<uint8_t>(brightness.current_value));
        }
        ddc_vcp_value_t contrast = {};
        if (ddc_get_vcp(&app->ddc, app->config.contrast_vcp, &contrast) == ESP_OK && contrast.present) {
            matter_update_level(app->matter.contrast_endpoint_id, static_cast<uint8_t>(contrast.current_value));
        }
        ddc_vcp_value_t input = {};
        if (read_current_input_value(app, &input) == ESP_OK && input.present) {
            uint8_t matched_mode = 0;
            if (try_find_mode_for_input(&app->config, input.current_value, &matched_mode)) {
                app->last_known_mode = matched_mode;
            }
            app->last_requested_input_valid = true;
            app->last_requested_input_value = static_cast<uint8_t>(input.current_value & 0xff);
        }
    } else {
        ESP_LOGI(TAG, "skipping initial DDC polling because no monitor is connected");
    }

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        matter_update_input_state(app->matter.input_endpoint_ids[index], false);
    }

    ESP_LOGI(TAG, "monitor=%s pnp=%s db_match=%d", app->config.monitor_name, app->config.pnp_id, app->config.db_match);
}

static void start_post_commissioning(app_context_t *app)
{
    if (webserver_start(&app->web) != ESP_OK) {
        ESP_LOGE(TAG, "webserver start failed");
    } else {
        app->webserver_started = true;
        esp_err_t mdns_err = refresh_web_mdns_alias(app);
        if (mdns_err == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "web mdns will publish once the network has an IP address");
        } else if (mdns_err != ESP_OK) {
            ESP_LOGW(TAG, "web mdns start failed: %s", esp_err_to_name(mdns_err));
            disable_web_mdns_alias();
        }
    }

    esp_err_t detect_err = detect_monitor(app, false);
    if (detect_err != ESP_OK) {
        ESP_LOGW(TAG, "post-commissioning detect failed: %s", esp_err_to_name(detect_err));
    }

    ESP_ERROR_CHECK(matter_sync_input_endpoints(&app->config));

    sync_runtime_state(app);
}

static void post_commission_task(void *arg)
{
    start_post_commissioning(static_cast<app_context_t *>(arg));
    vTaskDelete(NULL);
}

static esp_err_t schedule_post_commissioning(app_context_t *app)
{
    if (app->post_commission_started) {
        return ESP_OK;
    }

    app->post_commission_started = true;
    BaseType_t task_ok = xTaskCreate(post_commission_task, "post_commission", POST_COMMISSION_TASK_STACK_SIZE,
                                     app, 5, NULL);
    if (task_ok != pdPASS) {
        app->post_commission_started = false;
        ESP_LOGE(TAG, "failed to create post-commission task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t matter_commissioning_complete(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    return schedule_post_commissioning(app);
}

extern "C" void app_main(void)
{
    static app_context_t app = {};

    ESP_ERROR_CHECK(config_storage_init());
    ESP_ERROR_CHECK(ddc_init(&app.ddc, 21, 22, 100000));
    apply_safe_defaults(&app.config);
    preload_saved_config(&app.config);
    app.monitor_available = false;
    log_startup_i2c_probe(&app);

    matter_callbacks_t callbacks = {};
    callbacks.level_write = matter_level_write;
    callbacks.input_write = matter_input_write;
    callbacks.commissioning_complete = matter_commissioning_complete;
    callbacks.ctx = &app;
    ESP_ERROR_CHECK(matter_start(&app.config, &app.matter, &callbacks));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &network_ip_event_handler, &app));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &network_ip_event_handler, &app));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &network_ip_event_handler, &app));

    app.web.config = &app.config;
    app.web.profile = &app.profile;
    app.web.apply_config = apply_config_cb;
    app.web.test_input = test_input_cb;
    app.web.detect = detect_cb;
    app.web.refresh_db = refresh_db_cb;
    app.web.probe_inputs = probe_inputs_cb;
    app.web.get_level = get_level_cb;
    app.web.set_level = set_level_cb;
    app.web.open_commissioning_window = open_commissioning_window_cb;
    app.web.get_input_source_state = get_input_source_state_cb;
    app.web.ctx = &app;

    if (matter_is_commissioned()) {
        ESP_LOGI(TAG, "device already commissioned; open a new commissioning window from the web UI to add another controller");
        ESP_ERROR_CHECK(schedule_post_commissioning(&app));
    }
}
