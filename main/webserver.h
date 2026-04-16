#pragma once

#include "esp_err.h"

#include "config.h"
#include "monitor_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*web_apply_config_cb_t)(const display_config_t *config, void *ctx);
typedef esp_err_t (*web_test_input_cb_t)(uint8_t value, void *ctx);
typedef esp_err_t (*web_detect_cb_t)(void *ctx);
typedef esp_err_t (*web_refresh_db_cb_t)(void *ctx);

typedef struct {
    display_config_t *config;
    monitor_profile_t *profile;
    web_apply_config_cb_t apply_config;
    web_test_input_cb_t test_input;
    web_detect_cb_t detect;
    web_refresh_db_cb_t refresh_db;
    void *ctx;
} webserver_context_t;

esp_err_t webserver_start(webserver_context_t *ctx);

#ifdef __cplusplus
}
#endif
