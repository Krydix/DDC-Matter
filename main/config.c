#include "config.h"

#include <ctype.h>
#include <string.h>

#include "mccs.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "display_cfg"
#define KEY_USER_CONFIG "user_cfg"
#define KEY_CACHED_PROF "cache_prof"

typedef struct {
    uint8_t value;
    bool enabled;
    char name[INPUT_NAME_MAX_LEN];
} stored_input_slot_v1_t;

typedef struct {
    char pnp_id[PNP_ID_LEN];
    char monitor_name[MONITOR_NAME_MAX_LEN];
    bool profile_cached;
    bool user_override;
    bool db_match;
    uint8_t brightness_vcp;
    uint8_t contrast_vcp;
    stored_input_slot_v1_t inputs[INPUT_SLOT_COUNT];
} stored_display_config_v1_t;

typedef struct {
    uint8_t value;
    char name[INPUT_NAME_MAX_LEN];
} legacy_input_slot_t;

typedef struct {
    char pnp_id[PNP_ID_LEN];
    char monitor_name[MONITOR_NAME_MAX_LEN];
    bool profile_cached;
    bool user_override;
    bool db_match;
    uint8_t brightness_vcp;
    uint8_t contrast_vcp;
    legacy_input_slot_t inputs[INPUT_SLOT_COUNT];
} legacy_display_config_t;

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

esp_err_t config_normalize_wol_mac(const char *input, char *output, size_t output_len)
{
    char digits[12] = {0};
    size_t digit_count = 0;

    if (output == NULL || output_len < WOL_MAC_STR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    output[0] = '\0';
    if (input == NULL) {
        return ESP_OK;
    }

    for (size_t index = 0; input[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)input[index];
        if (isspace(ch) || ch == ':' || ch == '-') {
            continue;
        }

        if (!isxdigit(ch) || digit_count >= sizeof(digits)) {
            return ESP_ERR_INVALID_ARG;
        }

        digits[digit_count++] = (char)toupper(ch);
    }

    if (digit_count == 0) {
        return ESP_OK;
    }
    if (digit_count != sizeof(digits)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < sizeof(digits) / 2; ++index) {
        output[index * 3] = digits[index * 2];
        output[index * 3 + 1] = digits[index * 2 + 1];
        if (index + 1 < sizeof(digits) / 2) {
            output[index * 3 + 2] = ':';
        }
    }
    output[WOL_MAC_STR_LEN - 1] = '\0';
    return ESP_OK;
}

bool config_parse_wol_mac(const char *input, uint8_t mac[6])
{
    char normalized[WOL_MAC_STR_LEN] = {0};

    if (mac == NULL || config_normalize_wol_mac(input, normalized, sizeof(normalized)) != ESP_OK || normalized[0] == '\0') {
        return false;
    }

    for (size_t index = 0; index < 6; ++index) {
        int high = hex_nibble(normalized[index * 3]);
        int low = hex_nibble(normalized[index * 3 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        mac[index] = (uint8_t)((high << 4) | low);
    }

    return true;
}

static void normalize_config(display_config_t *config, bool legacy_format)
{
    if (config->brightness_vcp == 0) {
        config->brightness_vcp = 0x10;
    }
    if (config->contrast_vcp == 0) {
        config->contrast_vcp = 0x12;
    }

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        input_slot_t *slot = &config->inputs[index];
        bool has_mapping = slot->value != 0 || slot->name[0] != '\0';
        char normalized_mac[WOL_MAC_STR_LEN] = {0};

        if (legacy_format) {
            slot->enabled = has_mapping;
        }

        if (config_normalize_wol_mac(slot->wol_mac, normalized_mac, sizeof(normalized_mac)) == ESP_OK) {
            memcpy(slot->wol_mac, normalized_mac, sizeof(slot->wol_mac));
        } else {
            slot->wol_mac[0] = '\0';
        }

        if (!has_mapping) {
            slot->enabled = false;
            continue;
        }

        if (slot->name[0] == '\0') {
            strncpy(slot->name, mccs_input_label(slot->value), sizeof(slot->name) - 1);
            slot->name[sizeof(slot->name) - 1] = '\0';
        }
    }
}

static esp_err_t open_readonly_handle(nvs_handle_t *handle, bool *found)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *found = false;
        return ESP_OK;
    }
    return err;
}

esp_err_t config_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t read_blob(const char *key, void *value, size_t len, bool *found)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *found = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required = len;
    err = nvs_get_blob(handle, key, value, &required);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        nvs_close(handle);
        *found = false;
        return ESP_OK;
    }
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *found = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *found = (required == len);
    return *found ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t write_blob(const char *key, const void *value, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, key, value, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_load_user(display_config_t *config, bool *found)
{
    nvs_handle_t handle;
    size_t required = 0;

    memset(config, 0, sizeof(*config));
    esp_err_t err = open_readonly_handle(&handle, found);
    if (err != ESP_OK || !*found) {
        return err;
    }

    err = nvs_get_blob(handle, KEY_USER_CONFIG, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        *found = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    if (required == sizeof(*config)) {
        err = nvs_get_blob(handle, KEY_USER_CONFIG, config, &required);
        nvs_close(handle);
        if (err != ESP_OK) {
            return err;
        }
        normalize_config(config, false);
        *found = true;
        return ESP_OK;
    }

    if (required == sizeof(stored_display_config_v1_t)) {
        stored_display_config_v1_t stored = {};
        err = nvs_get_blob(handle, KEY_USER_CONFIG, &stored, &required);
        nvs_close(handle);
        if (err != ESP_OK) {
            return err;
        }

        memcpy(config->pnp_id, stored.pnp_id, sizeof(config->pnp_id));
        memcpy(config->monitor_name, stored.monitor_name, sizeof(config->monitor_name));
        config->profile_cached = stored.profile_cached;
        config->user_override = stored.user_override;
        config->db_match = stored.db_match;
        config->brightness_vcp = stored.brightness_vcp;
        config->contrast_vcp = stored.contrast_vcp;
        for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
            config->inputs[index].value = stored.inputs[index].value;
            config->inputs[index].enabled = stored.inputs[index].enabled;
            memcpy(config->inputs[index].name, stored.inputs[index].name, sizeof(config->inputs[index].name));
        }

        normalize_config(config, false);
        *found = true;
        return ESP_OK;
    }

    if (required == sizeof(legacy_display_config_t)) {
        legacy_display_config_t legacy = {};
        err = nvs_get_blob(handle, KEY_USER_CONFIG, &legacy, &required);
        nvs_close(handle);
        if (err != ESP_OK) {
            return err;
        }

        memcpy(config->pnp_id, legacy.pnp_id, sizeof(config->pnp_id));
        memcpy(config->monitor_name, legacy.monitor_name, sizeof(config->monitor_name));
        config->profile_cached = legacy.profile_cached;
        config->user_override = legacy.user_override;
        config->db_match = legacy.db_match;
        config->brightness_vcp = legacy.brightness_vcp;
        config->contrast_vcp = legacy.contrast_vcp;
        for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
            config->inputs[index].value = legacy.inputs[index].value;
            memcpy(config->inputs[index].name, legacy.inputs[index].name, sizeof(config->inputs[index].name));
        }

        normalize_config(config, true);
        *found = true;
        return ESP_OK;
    }

    nvs_close(handle);
    *found = false;
    return ESP_OK;
}

esp_err_t config_save_user(const display_config_t *config)
{
    return write_blob(KEY_USER_CONFIG, config, sizeof(*config));
}

esp_err_t config_load_cached_profile(cached_profile_t *profile, bool *found)
{
    memset(profile, 0, sizeof(*profile));
    return read_blob(KEY_CACHED_PROF, profile, sizeof(*profile), found);
}

esp_err_t config_save_cached_profile(const cached_profile_t *profile)
{
    return write_blob(KEY_CACHED_PROF, profile, sizeof(*profile));
}

esp_err_t config_clear_cached_profile(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, KEY_CACHED_PROF);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
