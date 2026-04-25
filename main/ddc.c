#include "ddc.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ddc";
static const uint8_t kStandardDdcDest = 0x51;
static const uint8_t kAlternateDdcDest = 0x50;
static const uint8_t kGetVcpResponseOpcode = 0x02;
static const uint8_t kStandardInputVcp = 0x60;
static const uint8_t kAlternateInputVcp = 0xF4;
static const TickType_t kGetVcpResponseDelay = pdMS_TO_TICKS(200);

static esp_err_t ddc_set_vcp_target_locked(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, uint16_t value);
static esp_err_t ddc_get_vcp_target_locked(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, ddc_vcp_value_t *value);
static esp_err_t ddc_lock_bus(ddc_bus_t *bus);
static void ddc_unlock_bus(ddc_bus_t *bus);

static bool parse_vcp_response(const uint8_t *response, size_t response_len, uint8_t vcp_code, ddc_vcp_value_t *value,
                               bool *unsupported)
{
    for (size_t offset = 0; offset + 8 <= response_len; ++offset) {
        if (response[offset] != kGetVcpResponseOpcode) {
            continue;
        }
        if (response[offset + 2] != vcp_code) {
            continue;
        }

        uint8_t result_code = response[offset + 1];
        if (result_code == 0x01) {
            *unsupported = true;
            return true;
        }
        if (result_code != 0x00) {
            continue;
        }

        value->present = true;
        value->maximum_value = ((uint16_t)response[offset + 4] << 8) | response[offset + 5];
        value->current_value = ((uint16_t)response[offset + 6] << 8) | response[offset + 7];
        return true;
    }

    return false;
}

static uint8_t ddc_checksum(uint8_t addr, const uint8_t *payload, size_t len)
{
    uint8_t checksum = addr;
    for (size_t i = 0; i < len; ++i) {
        checksum ^= payload[i];
    }
    return checksum;
}

bool ddc_input_source_value_is_usable(uint8_t vcp_code, const ddc_vcp_value_t *value)
{
    if (value == NULL || !value->present) {
        return false;
    }

    if (vcp_code == kStandardInputVcp && value->current_value == 0x0000) {
        return false;
    }

    if (vcp_code == kAlternateInputVcp && value->current_value == 0x0000 && value->maximum_value == 0x0000) {
        return false;
    }

    return true;
}

static esp_err_t ddc_lock_bus(ddc_bus_t *bus)
{
    if (bus == NULL || bus->lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(bus->lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void ddc_unlock_bus(ddc_bus_t *bus)
{
    if (bus != NULL && bus->lock != NULL) {
        xSemaphoreGive(bus->lock);
    }
}

esp_err_t ddc_init(ddc_bus_t *bus, int sda_gpio, int scl_gpio, uint32_t speed_hz)
{
    memset(bus, 0, sizeof(*bus));
    bus->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(bus->lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = scl_gpio,
        .sda_io_num = sda_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus->bus), TAG, "create bus failed");

    i2c_device_config_t edid_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DDC_EDID_ADDRESS,
        .scl_speed_hz = speed_hz,
    };
    i2c_device_config_t ddc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DDC_CI_ADDRESS,
        .scl_speed_hz = speed_hz,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus->bus, &edid_cfg, &bus->edid_dev), TAG, "add edid failed");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus->bus, &ddc_cfg, &bus->ddc_dev), TAG, "add ddc failed");
    return ESP_OK;
}

esp_err_t ddc_read_edid(ddc_bus_t *bus, uint8_t *buffer, size_t len)
{
    const uint8_t offset = 0x00;
    ESP_RETURN_ON_ERROR(ddc_lock_bus(bus), TAG, "lock failed");
    esp_err_t err = i2c_master_transmit_receive(bus->edid_dev, &offset, sizeof(offset), buffer, len, 1000);
    ddc_unlock_bus(bus);
    return err;
}

esp_err_t ddc_set_vcp(ddc_bus_t *bus, uint8_t vcp_code, uint16_t value)
{
    return ddc_set_vcp_for_destination(bus, kStandardDdcDest, vcp_code, value);
}

esp_err_t ddc_set_vcp_for_destination(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, uint16_t value)
{
    ESP_RETURN_ON_ERROR(ddc_lock_bus(bus), TAG, "lock failed");
    esp_err_t err = ddc_set_vcp_target_locked(bus, ddc_dest, vcp_code, value);
    ddc_unlock_bus(bus);
    return err;
}

static esp_err_t ddc_set_vcp_target_locked(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, uint16_t value)
{
    uint8_t packet[7] = {
        ddc_dest,
        0x84,
        0x03,
        vcp_code,
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)(value & 0xff),
        0x00,
    };
    packet[6] = ddc_checksum((DDC_CI_ADDRESS << 1), packet, sizeof(packet) - 1);
    return i2c_master_transmit(bus->ddc_dev, packet, sizeof(packet), 1000);
}

esp_err_t ddc_get_vcp(ddc_bus_t *bus, uint8_t vcp_code, ddc_vcp_value_t *value)
{
    return ddc_get_vcp_for_destination(bus, kStandardDdcDest, vcp_code, value);
}

esp_err_t ddc_get_vcp_for_destination(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, ddc_vcp_value_t *value)
{
    ESP_RETURN_ON_ERROR(ddc_lock_bus(bus), TAG, "lock failed");
    esp_err_t err = ddc_get_vcp_target_locked(bus, ddc_dest, vcp_code, value);
    ddc_unlock_bus(bus);
    return err;
}

static esp_err_t ddc_get_vcp_target_locked(ddc_bus_t *bus, uint8_t ddc_dest, uint8_t vcp_code, ddc_vcp_value_t *value)
{
    uint8_t request[5] = {ddc_dest, 0x82, 0x01, vcp_code, 0x00};
    uint8_t response[11] = {0};
    bool unsupported = false;

    memset(value, 0, sizeof(*value));
    request[4] = ddc_checksum((DDC_CI_ADDRESS << 1), request, sizeof(request) - 1);

    ESP_RETURN_ON_ERROR(i2c_master_transmit(bus->ddc_dev, request, sizeof(request), 1000), TAG, "get vcp tx failed");
    vTaskDelay(kGetVcpResponseDelay);
    ESP_RETURN_ON_ERROR(i2c_master_receive(bus->ddc_dev, response, sizeof(response), 1000), TAG, "get vcp rx failed");

    ESP_LOGD(TAG,
             "VCP 0x%02X raw response: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             vcp_code, response[0], response[1], response[2], response[3], response[4], response[5], response[6],
             response[7], response[8], response[9], response[10]);

    if (parse_vcp_response(response, sizeof(response), vcp_code, value, &unsupported)) {
        if (unsupported) {
            ESP_LOGW(TAG, "VCP 0x%02X reported unsupported", vcp_code);
            return ESP_ERR_NOT_SUPPORTED;
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "VCP 0x%02X reply did not contain a recognizable VCP response segment", vcp_code);
    return ESP_FAIL;
}

esp_err_t ddc_set_input_source(ddc_bus_t *bus, uint8_t value)
{
    if (value >= 0x80) {
        return ddc_set_vcp_for_destination(bus, kAlternateDdcDest, kAlternateInputVcp, value);
    }
    return ddc_set_vcp_for_destination(bus, kStandardDdcDest, kStandardInputVcp, value);
}

esp_err_t ddc_get_input_source_standard(ddc_bus_t *bus, ddc_vcp_value_t *value)
{
    return ddc_get_vcp_for_destination(bus, kStandardDdcDest, kStandardInputVcp, value);
}

esp_err_t ddc_get_input_source_alternate(ddc_bus_t *bus, ddc_vcp_value_t *value)
{
    return ddc_get_vcp_for_destination(bus, kAlternateDdcDest, kAlternateInputVcp, value);
}

esp_err_t ddc_get_input_source(ddc_bus_t *bus, ddc_vcp_value_t *value)
{
    ESP_RETURN_ON_ERROR(ddc_lock_bus(bus), TAG, "lock failed");

    esp_err_t err = ddc_get_vcp_target_locked(bus, kStandardDdcDest, kStandardInputVcp, value);
    if (err == ESP_OK && ddc_input_source_value_is_usable(kStandardInputVcp, value)) {
        ddc_unlock_bus(bus);
        return ESP_OK;
    }

    err = ddc_get_vcp_target_locked(bus, kAlternateDdcDest, kAlternateInputVcp, value);
    if (err == ESP_OK && ddc_input_source_value_is_usable(kAlternateInputVcp, value)) {
        ddc_unlock_bus(bus);
        return ESP_OK;
    }

    ddc_unlock_bus(bus);
    return err;
}

esp_err_t ddc_query_capabilities(ddc_bus_t *bus, char *buffer, size_t len)
{
    if (len < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    buffer[0] = '\0';
    ESP_RETURN_ON_ERROR(ddc_lock_bus(bus), TAG, "lock failed");

    while (offset < (len - 1)) {
        uint8_t request[7] = {
            0x51,
            0x84,
            0xF3,
            (uint8_t)((offset >> 8) & 0xff),
            (uint8_t)(offset & 0xff),
            0x00,
            0x00,
        };
        uint8_t response[40] = {0};
        request[5] = 0x00;
        request[6] = ddc_checksum((DDC_CI_ADDRESS << 1), request, sizeof(request) - 1);

        ESP_RETURN_ON_ERROR(i2c_master_transmit(bus->ddc_dev, request, sizeof(request), 1000), TAG, "caps tx failed");
        ESP_RETURN_ON_ERROR(i2c_master_receive(bus->ddc_dev, response, sizeof(response), 1000), TAG, "caps rx failed");

        size_t payload_len = response[1] > 3 ? response[1] - 3 : 0;
        if (payload_len == 0) {
            break;
        }
        if (offset + payload_len >= (len - 1)) {
            payload_len = len - 1 - offset;
        }
        memcpy(buffer + offset, &response[3], payload_len);
        offset += payload_len;
        buffer[offset] = '\0';

        if (strchr(buffer, ')') != NULL) {
            break;
        }
    }

    ddc_unlock_bus(bus);
    return offset > 0 ? ESP_OK : ESP_FAIL;
}

size_t ddc_extract_vcp_values(const char *caps, uint8_t feature_code, uint8_t *values, size_t max_values)
{
    char needle[8];
    snprintf(needle, sizeof(needle), "%02X(", feature_code);
    const char *start = strstr(caps, needle);
    if (start == NULL) {
        snprintf(needle, sizeof(needle), "%02x(", feature_code);
        start = strstr(caps, needle);
    }
    if (start == NULL) {
        return 0;
    }

    start += strlen(needle);
    size_t count = 0;
    while (*start != '\0' && *start != ')' && count < max_values) {
        while (*start == ' ') {
            ++start;
        }
        if (!isxdigit((unsigned char)start[0]) || !isxdigit((unsigned char)start[1])) {
            break;
        }
        unsigned int parsed = 0;
        if (sscanf(start, "%2x", &parsed) != 1) {
            break;
        }
        values[count++] = (uint8_t)parsed;
        start += 2;
        while (*start == ' ') {
            ++start;
        }
    }
    ESP_LOGI(TAG, "parsed %u values for feature 0x%02X", (unsigned int)count, feature_code);
    return count;
}
