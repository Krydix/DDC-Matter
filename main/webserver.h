#pragma once

#include "esp_err.h"

#include "config.h"
#include "ddc.h"
#include "monitor_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*web_apply_config_cb_t)(const display_config_t *config, void *ctx);
typedef esp_err_t (*web_test_input_cb_t)(uint8_t value, void *ctx);
typedef esp_err_t (*web_detect_cb_t)(void *ctx);
typedef esp_err_t (*web_refresh_db_cb_t)(void *ctx);
typedef esp_err_t (*web_probe_inputs_cb_t)(void *ctx);
typedef esp_err_t (*web_get_level_cb_t)(bool contrast, uint8_t vcp, ddc_vcp_value_t *value, void *ctx);
typedef esp_err_t (*web_set_level_cb_t)(bool contrast, uint8_t vcp, uint8_t value, void *ctx);

typedef struct {
    ddc_vcp_value_t standard;
    ddc_vcp_value_t alternate;
    ddc_vcp_value_t resolved;
    int matched_slot;
    char matched_name[INPUT_NAME_MAX_LEN];
} web_input_source_state_t;

typedef esp_err_t (*web_get_input_source_state_cb_t)(web_input_source_state_t *state, void *ctx);

typedef struct {
    display_config_t *config;
    monitor_profile_t *profile;
    web_apply_config_cb_t apply_config;
    web_test_input_cb_t test_input;
    web_detect_cb_t detect;
    web_refresh_db_cb_t refresh_db;
    web_probe_inputs_cb_t probe_inputs;
    web_get_level_cb_t get_level;
    web_set_level_cb_t set_level;
    web_get_input_source_state_cb_t get_input_source_state;
    void *ctx;
} webserver_context_t;

esp_err_t webserver_start(webserver_context_t *ctx);

#ifdef __cplusplus
}
#endif
