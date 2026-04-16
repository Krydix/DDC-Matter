#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *mccs_input_label(uint8_t value);
void mccs_fill_default_inputs(input_slot_t *slots, size_t count);
bool mccs_monitor_uses_lg_inputs(const char *monitor_name, const char *pnp_id);
void mccs_fill_default_inputs_for_display(input_slot_t *slots, size_t count, const char *monitor_name,
										  const char *pnp_id);
size_t mccs_get_probe_input_values(const char *monitor_name, const char *pnp_id, uint8_t *values, size_t max_values);

#ifdef __cplusplus
}
#endif
