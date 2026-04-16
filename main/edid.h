#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char pnp_id[PNP_ID_LEN];
    char monitor_name[MONITOR_NAME_MAX_LEN];
    bool has_name;
} edid_info_t;

esp_err_t edid_parse(const uint8_t *edid, size_t len, edid_info_t *info);

#ifdef __cplusplus
}
#endif
