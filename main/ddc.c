#include "ddc.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ddc";

static uint8_t ddc_checksum(uint8_t addr, const uint8_t *payload, size_t len)
{
    uint8_t checksum = addr;
    for (size_t i = 0; i < len; ++i) {
        checksum ^= payload[i];
    }
    return checksum;
}

esp_err_t ddc_init(ddc_bus_t *bus, int sda_gpio, int scl_gpio, uint32_t speed_hz)
{
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
    return i2c_master_transmit_receive(bus->edid_dev, &offset, sizeof(offset), buffer, len, 1000);
}

esp_err_t ddc_set_vcp(ddc_bus_t *bus, uint8_t vcp_code, uint16_t value)
{
    uint8_t packet[7] = {
        0x51,
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
    uint8_t request[5] = {0x51, 0x82, 0x01, vcp_code, 0x00};
    uint8_t response[11] = {0};
    request[4] = ddc_checksum((DDC_CI_ADDRESS << 1), request, sizeof(request) - 1);

    ESP_RETURN_ON_ERROR(i2c_master_transmit(bus->ddc_dev, request, sizeof(request), 1000), TAG, "get vcp tx failed");
    ESP_RETURN_ON_ERROR(i2c_master_receive(bus->ddc_dev, response, sizeof(response), 1000), TAG, "get vcp rx failed");

    value->present = (response[2] == 0x02) && (response[4] == 0x00);
    value->maximum_value = response[7];
    value->current_value = response[9];
    return value->present ? ESP_OK : ESP_FAIL;
}

esp_err_t ddc_query_capabilities(ddc_bus_t *bus, char *buffer, size_t len)
{
    if (len < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    buffer[0] = '\0';
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
