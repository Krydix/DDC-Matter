#include "mccs.h"

#include <string.h>

typedef struct {
    uint8_t value;
    const char *label;
} input_map_t;

static const input_map_t k_input_map[] = {
    {0x01, "VGA-1"},
    {0x02, "VGA-2"},
    {0x03, "DVI-1"},
    {0x04, "DVI-2"},
    {0x0F, "DP-1"},
    {0x10, "DP-2"},
    {0x11, "HDMI-1"},
    {0x12, "HDMI-2"},
    {0x13, "HDMI-3"},
    {0x14, "HDMI-4"},
};

const char *mccs_input_label(uint8_t value)
{
    for (size_t i = 0; i < sizeof(k_input_map) / sizeof(k_input_map[0]); ++i) {
        if (k_input_map[i].value == value) {
            return k_input_map[i].label;
        }
    }
    return "Unknown";
}

void mccs_fill_default_inputs(input_slot_t *slots, size_t count)
{
    static const uint8_t preferred[] = {0x0F, 0x10, 0x11, 0x12};
    for (size_t i = 0; i < count; ++i) {
        uint8_t value = preferred[i % (sizeof(preferred) / sizeof(preferred[0]))];
        slots[i].value = value;
        strncpy(slots[i].name, mccs_input_label(value), sizeof(slots[i].name) - 1);
        slots[i].name[sizeof(slots[i].name) - 1] = '\0';
    }
}
