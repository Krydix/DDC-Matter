#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool fetched;
    bool parsed;
    char url[96];
    char caps[PROFILE_CAPS_MAX_LEN];
} monitor_profile_t;

esp_err_t monitor_db_fetch_profile(const char *pnp_id, monitor_profile_t *profile);
size_t monitor_db_parse_input_values(const char *caps, input_slot_t *slots, size_t max_slots);
void monitor_db_apply_caps_to_inputs(const char *caps, input_slot_t *slots, size_t max_slots);

#ifdef __cplusplus
}
#endif
