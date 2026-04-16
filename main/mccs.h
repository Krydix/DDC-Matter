#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *mccs_input_label(uint8_t value);
void mccs_fill_default_inputs(input_slot_t *slots, size_t count);

#ifdef __cplusplus
}
#endif
