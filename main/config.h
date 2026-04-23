#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_SLOT_COUNT 5
#define MONITOR_NAME_MAX_LEN 32
#define PNP_ID_LEN 4
#define INPUT_NAME_MAX_LEN 24
#define PROFILE_CAPS_MAX_LEN 256

typedef struct {
    uint8_t value;
    bool enabled;
    char name[INPUT_NAME_MAX_LEN];
} input_slot_t;

typedef struct {
    char pnp_id[PNP_ID_LEN];
    char monitor_name[MONITOR_NAME_MAX_LEN];
    bool profile_cached;
    bool user_override;
    bool db_match;
    uint8_t brightness_vcp;
    uint8_t contrast_vcp;
    input_slot_t inputs[INPUT_SLOT_COUNT];
} display_config_t;

typedef struct {
    bool valid;
    bool from_cache;
    char pnp_id[PNP_ID_LEN];
    char caps[PROFILE_CAPS_MAX_LEN];
} cached_profile_t;

typedef enum {
    DETECTION_SOURCE_USER_CONFIG = 0,
    DETECTION_SOURCE_CACHE = 1,
    DETECTION_SOURCE_REMOTE_DB = 2,
    DETECTION_SOURCE_DDC_CAPS = 3,
    DETECTION_SOURCE_MCCS_DEFAULTS = 4,
} detection_source_t;

esp_err_t config_storage_init(void);
esp_err_t config_load_user(display_config_t *config, bool *found);
esp_err_t config_save_user(const display_config_t *config);
esp_err_t config_load_cached_profile(cached_profile_t *profile, bool *found);
esp_err_t config_save_cached_profile(const cached_profile_t *profile);
esp_err_t config_clear_cached_profile(void);

#ifdef __cplusplus
}
#endif
