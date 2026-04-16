#include <cstring>

#include "esp_check.h"
#include "esp_log.h"

extern "C" {
#include "config.h"
#include "ddc.h"
#include "edid.h"
#include "mccs.h"
#include "matter.h"
#include "monitor_db.h"
#include "webserver.h"
}

static const char *TAG = "app_main";

typedef struct {
    ddc_bus_t ddc;
    display_config_t config;
    cached_profile_t cache;
    monitor_profile_t profile;
    matter_runtime_t matter;
} app_context_t;

static void copy_inputs_from_slots(display_config_t *config, const input_slot_t *slots)
{
    for (size_t i = 0; i < INPUT_SLOT_COUNT; ++i) {
        config->inputs[i] = slots[i];
    }
}

static uint8_t find_mode_for_input(const display_config_t *config, uint8_t value)
{
    for (uint8_t i = 0; i < INPUT_SLOT_COUNT; ++i) {
        if (config->inputs[i].value == value) {
            return i;
        }
    }
    return 0;
}

static esp_err_t detect_monitor(app_context_t *app, bool force_remote_refresh)
{
    uint8_t edid[128] = {0};
    edid_info_t info = {};
    input_slot_t slots[INPUT_SLOT_COUNT] = {};
    display_config_t stored_user = {};
    bool user_found = false;

    std::memset(&app->profile, 0, sizeof(app->profile));
    ESP_RETURN_ON_ERROR(ddc_read_edid(&app->ddc, edid, sizeof(edid)), TAG, "edid read failed");
    ESP_RETURN_ON_ERROR(edid_parse(edid, sizeof(edid), &info), TAG, "edid parse failed");

    std::memset(&app->config, 0, sizeof(app->config));
    std::strncpy(app->config.pnp_id, info.pnp_id, sizeof(app->config.pnp_id) - 1);
    if (info.has_name) {
        std::strncpy(app->config.monitor_name, info.monitor_name, sizeof(app->config.monitor_name) - 1);
    } else {
        std::strncpy(app->config.monitor_name, "Display", sizeof(app->config.monitor_name) - 1);
    }
    app->config.brightness_vcp = 0x10;
    app->config.contrast_vcp = 0x12;

    ESP_RETURN_ON_ERROR(config_load_user(&stored_user, &user_found), TAG, "load user config failed");
    if (user_found && stored_user.user_override && !force_remote_refresh) {
        app->config = stored_user;
        std::strncpy(app->config.pnp_id, info.pnp_id, sizeof(app->config.pnp_id) - 1);
        if (info.has_name) {
            std::strncpy(app->config.monitor_name, info.monitor_name, sizeof(app->config.monitor_name) - 1);
        }
        ESP_LOGI(TAG, "using user override config");
        return ESP_OK;
    }

    bool cache_found = false;
    ESP_RETURN_ON_ERROR(config_load_cached_profile(&app->cache, &cache_found), TAG, "load cache failed");
    if (cache_found && app->cache.valid && std::strcmp(app->cache.pnp_id, info.pnp_id) == 0 && !force_remote_refresh) {
        monitor_db_apply_caps_to_inputs(app->cache.caps, slots, INPUT_SLOT_COUNT);
        copy_inputs_from_slots(&app->config, slots);
        app->config.profile_cached = true;
        app->config.db_match = true;
        ESP_LOGI(TAG, "using cached monitor profile");
        return ESP_OK;
    }

    if (monitor_db_fetch_profile(info.pnp_id, &app->profile) == ESP_OK && app->profile.parsed) {
        monitor_db_apply_caps_to_inputs(app->profile.caps, slots, INPUT_SLOT_COUNT);
        copy_inputs_from_slots(&app->config, slots);
        app->config.db_match = true;
        app->config.profile_cached = false;
        std::memset(&app->cache, 0, sizeof(app->cache));
        app->cache.valid = true;
        std::strncpy(app->cache.pnp_id, info.pnp_id, sizeof(app->cache.pnp_id) - 1);
        std::strncpy(app->cache.caps, app->profile.caps, sizeof(app->cache.caps) - 1);
        ESP_RETURN_ON_ERROR(config_save_cached_profile(&app->cache), TAG, "save cache failed");
        ESP_LOGI(TAG, "using remote ddccontrol-db profile %s", app->profile.url);
        return ESP_OK;
    }

    char caps[PROFILE_CAPS_MAX_LEN] = {0};
    if (ddc_query_capabilities(&app->ddc, caps, sizeof(caps)) == ESP_OK) {
        size_t count = monitor_db_parse_input_values(caps, slots, INPUT_SLOT_COUNT);
        if (count > 0) {
            if (count < INPUT_SLOT_COUNT) {
                mccs_fill_default_inputs(&slots[count], INPUT_SLOT_COUNT - count);
            }
            copy_inputs_from_slots(&app->config, slots);
            app->config.db_match = false;
            app->config.profile_cached = false;
            ESP_LOGI(TAG, "using direct DDC capabilities");
            return ESP_OK;
        }
    }

    mccs_fill_default_inputs(app->config.inputs, INPUT_SLOT_COUNT);
    app->config.db_match = false;
    app->config.profile_cached = false;
    ESP_LOGI(TAG, "using MCCS defaults");
    return ESP_OK;
}

static esp_err_t set_input_value(app_context_t *app, uint8_t input_value)
{
    return ddc_set_vcp(&app->ddc, 0x60, input_value);
}

static esp_err_t matter_level_write(uint16_t endpoint_id, uint8_t level, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    uint8_t vcp = (endpoint_id == app->matter.contrast_endpoint_id) ? app->config.contrast_vcp : app->config.brightness_vcp;
    return ddc_set_vcp(&app->ddc, vcp, level);
}

static esp_err_t matter_mode_write(uint8_t mode, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    if (mode >= INPUT_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_input_value(app, app->config.inputs[mode].value);
}

static esp_err_t apply_config_cb(const display_config_t *config, void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    app->config = *config;
    ESP_RETURN_ON_ERROR(config_save_user(&app->config), TAG, "save user config failed");
    return ESP_OK;
}

static esp_err_t test_input_cb(uint8_t value, void *ctx)
{
    return set_input_value(static_cast<app_context_t *>(ctx), value);
}

static esp_err_t detect_cb(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    return detect_monitor(app, false);
}

static esp_err_t refresh_db_cb(void *ctx)
{
    app_context_t *app = static_cast<app_context_t *>(ctx);
    return detect_monitor(app, true);
}

extern "C" void app_main(void)
{
    static app_context_t app = {};
    static webserver_context_t web_ctx = {};

    ESP_ERROR_CHECK(config_storage_init());
    ESP_ERROR_CHECK(ddc_init(&app.ddc, 21, 22, 100000));
    ESP_ERROR_CHECK(detect_monitor(&app, false));

    matter_callbacks_t callbacks = {};
    callbacks.level_write = matter_level_write;
    callbacks.mode_write = matter_mode_write;
    callbacks.ctx = &app;
    ESP_ERROR_CHECK(matter_start(&app.config, &app.matter, &callbacks));

    web_ctx.config = &app.config;
    web_ctx.profile = &app.profile;
    web_ctx.apply_config = apply_config_cb;
    web_ctx.test_input = test_input_cb;
    web_ctx.detect = detect_cb;
    web_ctx.refresh_db = refresh_db_cb;
    web_ctx.ctx = &app;
    ESP_ERROR_CHECK(webserver_start(&web_ctx));

    ddc_vcp_value_t brightness = {};
    if (ddc_get_vcp(&app.ddc, app.config.brightness_vcp, &brightness) == ESP_OK && brightness.present) {
        matter_update_level(app.matter.brightness_endpoint_id, brightness.current_value);
    }
    ddc_vcp_value_t contrast = {};
    if (ddc_get_vcp(&app.ddc, app.config.contrast_vcp, &contrast) == ESP_OK && contrast.present) {
        matter_update_level(app.matter.contrast_endpoint_id, contrast.current_value);
    }
    ddc_vcp_value_t input = {};
    uint8_t current_mode = 0;
    if (ddc_get_vcp(&app.ddc, 0x60, &input) == ESP_OK && input.present) {
        current_mode = find_mode_for_input(&app.config, input.current_value);
    }
    matter_update_mode(app.matter.input_endpoint_id, current_mode);

    ESP_LOGI(TAG, "monitor=%s pnp=%s db_match=%d", app.config.monitor_name, app.config.pnp_id, app.config.db_match);
}
