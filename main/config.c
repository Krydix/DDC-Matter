#include "config.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "display_cfg"
#define KEY_USER_CONFIG "user_cfg"
#define KEY_CACHED_PROF "cache_prof"

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
    memset(config, 0, sizeof(*config));
    return read_blob(KEY_USER_CONFIG, config, sizeof(*config), found);
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
