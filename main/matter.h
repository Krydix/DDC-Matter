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
    uint16_t input_endpoint_ids[INPUT_SLOT_COUNT];
} matter_runtime_t;

typedef esp_err_t (*matter_level_write_cb_t)(uint16_t endpoint_id, uint8_t level, void *ctx);
typedef esp_err_t (*matter_input_write_cb_t)(uint16_t endpoint_id, bool on, void *ctx);
typedef esp_err_t (*matter_commissioning_complete_cb_t)(void *ctx);

typedef struct {
    matter_level_write_cb_t level_write;
    matter_input_write_cb_t input_write;
    /* Called once when the device completes commissioning. */
    matter_commissioning_complete_cb_t commissioning_complete;
    void *ctx;
} matter_callbacks_t;

esp_err_t matter_start(const display_config_t *config, matter_runtime_t *runtime, const matter_callbacks_t *callbacks);
esp_err_t matter_update_level(uint16_t endpoint_id, uint8_t level);
esp_err_t matter_update_input_state(uint16_t endpoint_id, bool on);
esp_err_t matter_sync_input_endpoints(const display_config_t *config);
esp_err_t matter_open_basic_commissioning_window(void);
bool matter_is_commissioned(void);

#ifdef __cplusplus
}
#endif
