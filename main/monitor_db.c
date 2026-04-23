#include "monitor_db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "ddc.h"
#include "mccs.h"

static esp_err_t http_collect(esp_http_client_handle_t client, char *out, size_t out_len)
{
    int total = 0;
    while (true) {
        int read = esp_http_client_read(client, out + total, out_len - 1 - total);
        if (read < 0) {
            return ESP_FAIL;
        }
        if (read == 0) {
            break;
        }
        total += read;
        if ((size_t)total >= out_len - 1) {
            break;
        }
    }
    out[total] = '\0';
    return total > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t monitor_db_fetch_profile(const char *pnp_id, monitor_profile_t *profile)
{
    esp_err_t ret = ESP_OK;
    memset(profile, 0, sizeof(*profile));
    snprintf(profile->url, sizeof(profile->url),
             "https://raw.githubusercontent.com/ddccontrol/ddccontrol-db/master/db/monitor/u%s.xml",
             pnp_id);

    char xml[2048] = {0};
    esp_http_client_config_t config = {
        .url = profile->url,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "esp32-display-switcher/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, "monitor_db", "client init failed");
    ESP_GOTO_ON_ERROR(esp_http_client_open(client, 0), fail, "monitor_db", "open failed");
    ESP_GOTO_ON_ERROR(http_collect(client, xml, sizeof(xml)), fail, "monitor_db", "read failed");

    const char *caps_attr = strstr(xml, "caps add=\"");
    if (caps_attr == NULL) {
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_FOUND, fail, "monitor_db", "caps add missing");
    }
    caps_attr += strlen("caps add=\"");
    const char *end = strchr(caps_attr, '"');
    ESP_GOTO_ON_FALSE(end != NULL, ESP_ERR_INVALID_RESPONSE, fail, "monitor_db", "caps add malformed");

    size_t caps_len = (size_t)(end - caps_attr);
    if (caps_len >= sizeof(profile->caps)) {
        caps_len = sizeof(profile->caps) - 1;
    }
    memcpy(profile->caps, caps_attr, caps_len);
    profile->caps[caps_len] = '\0';
    profile->fetched = true;
    profile->parsed = caps_len > 0;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;

fail:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

size_t monitor_db_parse_input_values(const char *caps, input_slot_t *slots, size_t max_slots)
{
    uint8_t values[INPUT_SLOT_COUNT] = {0};
    size_t count = ddc_extract_vcp_values(caps, 0x60, values, max_slots);
    for (size_t i = 0; i < count; ++i) {
        slots[i].value = values[i];
        slots[i].enabled = true;
        strncpy(slots[i].name, mccs_input_label(values[i]), sizeof(slots[i].name) - 1);
        slots[i].name[sizeof(slots[i].name) - 1] = '\0';
    }
    return count;
}

void monitor_db_apply_caps_to_inputs(const char *caps, input_slot_t *slots, size_t max_slots)
{
    size_t count = monitor_db_parse_input_values(caps, slots, max_slots);
    if (count < max_slots) {
        mccs_fill_default_inputs(&slots[count], max_slots - count);
    }
}
