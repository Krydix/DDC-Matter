#include "edid.h"

#include <ctype.h>
#include <string.h>

static char decode_edid_char(uint8_t value)
{
    return (char)('A' + value - 1);
}

static void trim_ascii(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

esp_err_t edid_parse(const uint8_t *edid, size_t len, edid_info_t *info)
{
    if (len < 128) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(info, 0, sizeof(*info));
    uint16_t manufacturer = ((uint16_t)edid[8] << 8) | edid[9];
    info->pnp_id[0] = decode_edid_char((manufacturer >> 10) & 0x1f);
    info->pnp_id[1] = decode_edid_char((manufacturer >> 5) & 0x1f);
    info->pnp_id[2] = decode_edid_char(manufacturer & 0x1f);
    info->pnp_id[3] = '\0';

    for (size_t offset = 54; offset + 18 <= 126; offset += 18) {
        const uint8_t *desc = &edid[offset];
        if (desc[0] == 0x00 && desc[1] == 0x00 && desc[2] == 0x00 && desc[3] == 0xfc) {
            size_t out = 0;
            for (size_t i = 5; i < 18 && out < sizeof(info->monitor_name) - 1; ++i) {
                char c = (char)desc[i];
                if (c == '\n' || c == '\r' || c == '\0') {
                    break;
                }
                info->monitor_name[out++] = isprint((unsigned char)c) ? c : ' ';
            }
            info->monitor_name[out] = '\0';
            trim_ascii(info->monitor_name);
            info->has_name = (out > 0);
            break;
        }
    }

    return ESP_OK;
}
