#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DDC_EDID_ADDRESS 0x50
#define DDC_CI_ADDRESS 0x37

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t edid_dev;
    i2c_master_dev_handle_t ddc_dev;
} ddc_bus_t;

typedef struct {
    uint8_t current_value;
    uint8_t maximum_value;
    bool present;
} ddc_vcp_value_t;

esp_err_t ddc_init(ddc_bus_t *bus, int sda_gpio, int scl_gpio, uint32_t speed_hz);
esp_err_t ddc_read_edid(ddc_bus_t *bus, uint8_t *buffer, size_t len);
esp_err_t ddc_set_vcp(ddc_bus_t *bus, uint8_t vcp_code, uint16_t value);
esp_err_t ddc_get_vcp(ddc_bus_t *bus, uint8_t vcp_code, ddc_vcp_value_t *value);
esp_err_t ddc_query_capabilities(ddc_bus_t *bus, char *buffer, size_t len);
size_t ddc_extract_vcp_values(const char *caps, uint8_t feature_code, uint8_t *values, size_t max_values);

#ifdef __cplusplus
}
#endif
