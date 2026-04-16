#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char device_name[MONITOR_NAME_MAX_LEN];
    uint16_t brightness_endpoint_id;
    uint16_t contrast_endpoint_id;
    uint16_t input_endpoint_id;
} matter_runtime_t;

typedef esp_err_t (*matter_level_write_cb_t)(uint16_t endpoint_id, uint8_t level, void *ctx);
typedef esp_err_t (*matter_mode_write_cb_t)(uint8_t mode, void *ctx);

typedef struct {
    matter_level_write_cb_t level_write;
    matter_mode_write_cb_t mode_write;
    void *ctx;
} matter_callbacks_t;

esp_err_t matter_start(const display_config_t *config, matter_runtime_t *runtime, const matter_callbacks_t *callbacks);
esp_err_t matter_update_level(uint16_t endpoint_id, uint8_t level);
esp_err_t matter_update_mode(uint16_t endpoint_id, uint8_t mode);

#ifdef __cplusplus
}
#endif
