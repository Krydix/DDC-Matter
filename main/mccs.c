#include "mccs.h"

#include <ctype.h>
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
    {0x1B, "USB-C / DP-3"},
    {0x13, "HDMI-3"},
    {0x14, "HDMI-4"},
    {0x19, "USB-C-1"},
    {0x1C, "USB-C-3"},
    {0x90, "HDMI-1 (LG)"},
    {0x91, "HDMI-2 (LG)"},
    {0x92, "HDMI-3 (LG)"},
    {0x93, "HDMI-4 (LG)"},
    {0xC0, "DP-3 (LG)"},
    {0xC1, "DP-4 (LG)"},
    {0xD0, "DP-1 (LG)"},
    {0xD1, "DP-2 (LG)"},
    {0xD2, "USB-C-1 (LG)"},
    {0xD3, "USB-C-2 (LG)"},
    {0xE0, "USB-C-3 (LG)"},
    {0xE1, "USB-C-4 (LG)"},
};

static const uint8_t k_standard_probe_values[] = {0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x19, 0x1B, 0x1C};
static const uint8_t k_lg_probe_values[] = {0xD0, 0xD1, 0x90, 0x91, 0xD2, 0xC0, 0xC1, 0xD3, 0xE0, 0xE1, 0x92, 0x93};

static void fill_inputs_from_values(input_slot_t *slots, size_t count, const uint8_t *values, size_t value_count)
{
    for (size_t i = 0; i < count; ++i) {
        uint8_t value = values[i % value_count];
        slots[i].value = value;
        strncpy(slots[i].name, mccs_input_label(value), sizeof(slots[i].name) - 1);
        slots[i].name[sizeof(slots[i].name) - 1] = '\0';
    }
}

static bool ascii_starts_with_ignore_case(const char *value, const char *prefix)
{
    if (value == NULL || prefix == NULL) {
        return false;
    }

    while (*prefix != '\0') {
        if (*value == '\0') {
            return false;
        }
        if (toupper((unsigned char)*value) != toupper((unsigned char)*prefix)) {
            return false;
        }
        ++value;
        ++prefix;
    }
    return true;
}

static bool ascii_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (toupper((unsigned char)*left) != toupper((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }

    return *left == '\0' && *right == '\0';
}

const char *mccs_input_label(uint8_t value)
{
    for (size_t i = 0; i < sizeof(k_input_map) / sizeof(k_input_map[0]); ++i) {
        if (k_input_map[i].value == value) {
            return k_input_map[i].label;
        }
    }
    return "Unknown";
}

bool mccs_monitor_uses_lg_inputs(const char *monitor_name, const char *pnp_id)
{
    return ascii_starts_with_ignore_case(monitor_name, "LG ") || ascii_equals_ignore_case(pnp_id, "GSM");
}

bool mccs_input_matches_lg_fingerprint(uint8_t input_value, uint8_t fingerprint)
{
    switch (fingerprint) {
        case 0x00:
            return input_value == 0xD1;
        case 0x03:
            return input_value == 0x0F || input_value == 0x11 || input_value == 0x12 || input_value == 0x90 ||
                   input_value == 0x91 || input_value == 0xD0;
        case 0xFF:
            return input_value == 0xC0 || input_value == 0xC1 || input_value == 0xD2 || input_value == 0xD3 ||
                   input_value == 0xE0 || input_value == 0xE1;
        default:
            return false;
    }
}

void mccs_fill_default_inputs(input_slot_t *slots, size_t count)
{
    static const uint8_t preferred[] = {0x0F, 0x10, 0x11, 0x12, 0x1B};
    fill_inputs_from_values(slots, count, preferred, sizeof(preferred) / sizeof(preferred[0]));
}

void mccs_fill_default_inputs_for_display(input_slot_t *slots, size_t count, const char *monitor_name, const char *pnp_id)
{
    static const uint8_t preferred_lg[] = {0xD0, 0xD1, 0x90, 0x91, 0xD2};

    if (mccs_monitor_uses_lg_inputs(monitor_name, pnp_id)) {
        fill_inputs_from_values(slots, count, preferred_lg, sizeof(preferred_lg) / sizeof(preferred_lg[0]));
        return;
    }

    mccs_fill_default_inputs(slots, count);
}

size_t mccs_get_probe_input_values(const char *monitor_name, const char *pnp_id, uint8_t *values, size_t max_values)
{
    size_t count = 0;

    for (size_t index = 0; index < sizeof(k_standard_probe_values) / sizeof(k_standard_probe_values[0]) && count < max_values;
         ++index) {
        values[count++] = k_standard_probe_values[index];
    }

    if (!mccs_monitor_uses_lg_inputs(monitor_name, pnp_id)) {
        return count;
    }

    for (size_t index = 0; index < sizeof(k_lg_probe_values) / sizeof(k_lg_probe_values[0]) && count < max_values; ++index) {
        values[count++] = k_lg_probe_values[index];
    }

    return count;
}
