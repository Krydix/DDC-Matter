#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "ddc.h"
#include "edid.h"
#include "mccs.h"
}

static constexpr uart_port_t kConsoleUart = UART_NUM_0;
static constexpr int kConsoleRxBuffer = 2048;
static constexpr size_t kCommandBufferSize = 256;
static constexpr size_t kMaxTokens = 32;
static constexpr size_t kMaxProbeValues = 24;
static constexpr size_t kRawReadLimit = 128;
static constexpr TickType_t kProbeSettleDelay = pdMS_TO_TICKS(1200);

typedef struct {
    ddc_bus_t ddc;
    edid_info_t edid;
    bool edid_valid;
} debug_context_t;

static void print_prompt(void)
{
    printf("dbg> ");
    fflush(stdout);
}

static bool parse_u16(const char *text, uint16_t *value)
{
    if (text == NULL || *text == '\0') {
        return false;
    }

    char *end = NULL;
    int base = 10;
    if (text[0] == 'x' || text[0] == 'X') {
        ++text;
        base = 16;
    } else if (strlen(text) > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }

    unsigned long parsed = strtoul(text, &end, base);
    if (end == NULL || *end != '\0' || parsed > 0xffffUL) {
        return false;
    }

    *value = (uint16_t)parsed;
    return true;
}

static int tokenize(char *line, char **tokens, size_t max_tokens)
{
    size_t count = 0;
    char *save = NULL;
    for (char *token = strtok_r(line, " \t", &save); token != NULL; token = strtok_r(NULL, " \t", &save)) {
        if (count >= max_tokens) {
            break;
        }
        tokens[count++] = token;
    }
    return (int)count;
}

static void print_bytes(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        printf("%s%02X", (i == 0) ? "" : " ", bytes[i]);
    }
    printf("\n");
}

static esp_err_t refresh_edid(debug_context_t *ctx)
{
    uint8_t edid[128] = {};
    esp_err_t err = ddc_read_edid(&ctx->ddc, edid, sizeof(edid));
    if (err != ESP_OK) {
        ctx->edid_valid = false;
        return err;
    }

    err = edid_parse(edid, sizeof(edid), &ctx->edid);
    ctx->edid_valid = (err == ESP_OK);
    return err;
}

static void print_edid_info(debug_context_t *ctx)
{
    esp_err_t err = refresh_edid(ctx);
    if (err != ESP_OK) {
        printf("EDID read failed: %s\n", esp_err_to_name(err));
        return;
    }

    printf("PnP ID: %s\n", ctx->edid.pnp_id);
    printf("Monitor: %s\n", ctx->edid.has_name ? ctx->edid.monitor_name : "<none>");
    printf("LG-family: %s\n", mccs_monitor_uses_lg_inputs(ctx->edid.monitor_name, ctx->edid.pnp_id) ? "yes" : "no");
}

static void print_vcp_result(const char *label, esp_err_t err, const ddc_vcp_value_t *value)
{
    if (err != ESP_OK) {
        printf("%s: error %s\n", label, esp_err_to_name(err));
        return;
    }
    if (!value->present) {
        printf("%s: not present\n", label);
        return;
    }
    printf("%s: current=0x%04X (%u) max=0x%04X (%u)\n", label, value->current_value, value->current_value,
           value->maximum_value, value->maximum_value);
}

static void print_input_state(debug_context_t *ctx)
{
    ddc_vcp_value_t standard = {};
    ddc_vcp_value_t alternate = {};
    ddc_vcp_value_t resolved = {};
    esp_err_t standard_err = ddc_get_input_source_standard(&ctx->ddc, &standard);
    esp_err_t alternate_err = ddc_get_input_source_alternate(&ctx->ddc, &alternate);
    esp_err_t resolved_err = ddc_get_input_source(&ctx->ddc, &resolved);

    print_vcp_result("standard 0x60 via 0x51", standard_err, &standard);
    print_vcp_result("alternate 0xF4 via 0x50", alternate_err, &alternate);
    print_vcp_result("resolved current path", resolved_err, &resolved);
}

static i2c_master_dev_handle_t get_device_for_address(debug_context_t *ctx, uint16_t address)
{
    if (address == DDC_CI_ADDRESS) {
        return ctx->ddc.ddc_dev;
    }
    if (address == DDC_EDID_ADDRESS) {
        return ctx->ddc.edid_dev;
    }
    return NULL;
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  help\n");
    printf("  edid\n");
    printf("  caps\n");
    printf("  input\n");
    printf("  set-input <value>\n");
    printf("  probe-known\n");
    printf("  probe <value> [value ...]\n");
    printf("  ddc-get <dest> <vcp>\n");
    printf("  ddc-set <dest> <vcp> <value>\n");
    printf("  i2c-tx <addr> <byte> [byte ...]\n");
    printf("  i2c-rx <addr> <len>\n");
    printf("  i2c-xfer <addr> <rx-len> <byte> [byte ...]\n");
    printf("Notes:\n");
    printf("  ddc-get/ddc-set use DDC destination bytes like 0x51 or 0x50.\n");
    printf("  i2c-* uses physical bus addresses 0x37 (DDC/CI) or 0x50 (EDID).\n");
}

static void handle_caps(debug_context_t *ctx)
{
    char caps[PROFILE_CAPS_MAX_LEN] = {};
    esp_err_t err = ddc_query_capabilities(&ctx->ddc, caps, sizeof(caps));
    if (err != ESP_OK) {
        printf("capabilities query failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("%s\n", caps);
}

static void handle_set_input(debug_context_t *ctx, const char *value_text)
{
    uint16_t value = 0;
    if (!parse_u16(value_text, &value) || value > 0xff) {
        printf("invalid input value\n");
        return;
    }

    esp_err_t err = ddc_set_input_source(&ctx->ddc, (uint8_t)value);
    if (err != ESP_OK) {
        printf("set-input failed: %s\n", esp_err_to_name(err));
        return;
    }

    vTaskDelay(kProbeSettleDelay);
    print_input_state(ctx);
}

static bool parse_value_list(char **tokens, int token_count, int start_index, uint8_t *values, size_t *count)
{
    size_t out = 0;
    for (int i = start_index; i < token_count; ++i) {
        uint16_t parsed = 0;
        if (!parse_u16(tokens[i], &parsed) || parsed > 0xff || out >= kMaxProbeValues) {
            return false;
        }
        values[out++] = (uint8_t)parsed;
    }
    *count = out;
    return out > 0;
}

static void run_probe(debug_context_t *ctx, const uint8_t *values, size_t count)
{
    ddc_vcp_value_t original = {};
    esp_err_t original_err = ddc_get_input_source(&ctx->ddc, &original);
    if (original_err != ESP_OK || !original.present) {
        printf("cannot capture original input: %s\n", esp_err_to_name(original_err));
        return;
    }

    printf("original input: 0x%02X\n", (unsigned int)(original.current_value & 0xff));
    for (size_t i = 0; i < count; ++i) {
        uint8_t requested = values[i];
        printf("-- request 0x%02X (%s)\n", requested, mccs_input_label(requested));
        esp_err_t err = ddc_set_input_source(&ctx->ddc, requested);
        if (err != ESP_OK) {
            printf("write failed: %s\n", esp_err_to_name(err));
            continue;
        }

        vTaskDelay(kProbeSettleDelay);
        print_input_state(ctx);
    }

    esp_err_t restore_err = ddc_set_input_source(&ctx->ddc, (uint8_t)(original.current_value & 0xff));
    if (restore_err == ESP_OK) {
        vTaskDelay(kProbeSettleDelay);
        printf("restored original input\n");
        print_input_state(ctx);
    } else {
        printf("restore failed: %s\n", esp_err_to_name(restore_err));
    }
}

static void handle_probe_known(debug_context_t *ctx)
{
    if (refresh_edid(ctx) != ESP_OK) {
        printf("probe-known requires a readable EDID\n");
        return;
    }

    uint8_t candidates[kMaxProbeValues] = {};
    size_t count = mccs_get_probe_input_values(ctx->edid.monitor_name, ctx->edid.pnp_id, candidates,
                                               sizeof(candidates) / sizeof(candidates[0]));
    if (count == 0) {
        printf("no probe candidates\n");
        return;
    }

    run_probe(ctx, candidates, count);
}

static void handle_ddc_get(debug_context_t *ctx, const char *dest_text, const char *vcp_text)
{
    uint16_t dest = 0;
    uint16_t vcp = 0;
    ddc_vcp_value_t value = {};
    if (!parse_u16(dest_text, &dest) || !parse_u16(vcp_text, &vcp) || dest > 0xff || vcp > 0xff) {
        printf("usage: ddc-get <dest> <vcp>\n");
        return;
    }

    esp_err_t err = ddc_get_vcp_for_destination(&ctx->ddc, (uint8_t)dest, (uint8_t)vcp, &value);
    print_vcp_result("ddc-get", err, &value);
}

static void handle_ddc_set(debug_context_t *ctx, const char *dest_text, const char *vcp_text, const char *value_text)
{
    uint16_t dest = 0;
    uint16_t vcp = 0;
    uint16_t value = 0;
    if (!parse_u16(dest_text, &dest) || !parse_u16(vcp_text, &vcp) || !parse_u16(value_text, &value) || dest > 0xff ||
        vcp > 0xff) {
        printf("usage: ddc-set <dest> <vcp> <value>\n");
        return;
    }

    esp_err_t err = ddc_set_vcp_for_destination(&ctx->ddc, (uint8_t)dest, (uint8_t)vcp, value);
    if (err != ESP_OK) {
        printf("ddc-set failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("ok\n");
}

static void handle_i2c_tx(debug_context_t *ctx, char **tokens, int token_count)
{
    uint16_t address = 0;
    if (token_count < 3 || !parse_u16(tokens[1], &address)) {
        printf("usage: i2c-tx <addr> <byte> [byte ...]\n");
        return;
    }

    i2c_master_dev_handle_t device = get_device_for_address(ctx, address);
    if (device == NULL) {
        printf("unsupported address; use 0x37 or 0x50\n");
        return;
    }

    uint8_t payload[kMaxTokens] = {};
    size_t payload_len = 0;
    for (int i = 2; i < token_count; ++i) {
        uint16_t parsed = 0;
        if (!parse_u16(tokens[i], &parsed) || parsed > 0xff) {
            printf("invalid byte: %s\n", tokens[i]);
            return;
        }
        payload[payload_len++] = (uint8_t)parsed;
    }

    esp_err_t err = i2c_master_transmit(device, payload, payload_len, 1000);
    if (err != ESP_OK) {
        printf("i2c-tx failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("ok\n");
}

static void handle_i2c_rx(debug_context_t *ctx, const char *addr_text, const char *len_text)
{
    uint16_t address = 0;
    uint16_t length = 0;
    if (!parse_u16(addr_text, &address) || !parse_u16(len_text, &length) || length == 0 || length > kRawReadLimit) {
        printf("usage: i2c-rx <addr> <len<=128>\n");
        return;
    }

    i2c_master_dev_handle_t device = get_device_for_address(ctx, address);
    if (device == NULL) {
        printf("unsupported address; use 0x37 or 0x50\n");
        return;
    }

    uint8_t buffer[kRawReadLimit] = {};
    esp_err_t err = i2c_master_receive(device, buffer, length, 1000);
    if (err != ESP_OK) {
        printf("i2c-rx failed: %s\n", esp_err_to_name(err));
        return;
    }

    print_bytes(buffer, length);
}

static void handle_i2c_xfer(debug_context_t *ctx, char **tokens, int token_count)
{
    uint16_t address = 0;
    uint16_t length = 0;
    if (token_count < 4 || !parse_u16(tokens[1], &address) || !parse_u16(tokens[2], &length) || length > kRawReadLimit) {
        printf("usage: i2c-xfer <addr> <rx-len<=128> <byte> [byte ...]\n");
        return;
    }

    i2c_master_dev_handle_t device = get_device_for_address(ctx, address);
    if (device == NULL) {
        printf("unsupported address; use 0x37 or 0x50\n");
        return;
    }

    uint8_t tx[kMaxTokens] = {};
    size_t tx_len = 0;
    for (int i = 3; i < token_count; ++i) {
        uint16_t parsed = 0;
        if (!parse_u16(tokens[i], &parsed) || parsed > 0xff) {
            printf("invalid byte: %s\n", tokens[i]);
            return;
        }
        tx[tx_len++] = (uint8_t)parsed;
    }

    uint8_t rx[kRawReadLimit] = {};
    esp_err_t err = i2c_master_transmit(device, tx, tx_len, 1000);
    if (err != ESP_OK) {
        printf("i2c-xfer tx failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (length == 0) {
        printf("ok\n");
        return;
    }

    err = i2c_master_receive(device, rx, length, 1000);
    if (err != ESP_OK) {
        printf("i2c-xfer rx failed: %s\n", esp_err_to_name(err));
        return;
    }
    print_bytes(rx, length);
}

static void execute_command(debug_context_t *ctx, char *line)
{
    char *tokens[kMaxTokens] = {};
    int token_count = tokenize(line, tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (token_count == 0) {
        return;
    }

    if (strcmp(tokens[0], "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(tokens[0], "edid") == 0) {
        print_edid_info(ctx);
        return;
    }
    if (strcmp(tokens[0], "caps") == 0) {
        handle_caps(ctx);
        return;
    }
    if (strcmp(tokens[0], "input") == 0) {
        print_input_state(ctx);
        return;
    }
    if (strcmp(tokens[0], "set-input") == 0 && token_count == 2) {
        handle_set_input(ctx, tokens[1]);
        return;
    }
    if (strcmp(tokens[0], "probe-known") == 0) {
        handle_probe_known(ctx);
        return;
    }
    if (strcmp(tokens[0], "probe") == 0 && token_count >= 2) {
        uint8_t values[kMaxProbeValues] = {};
        size_t count = 0;
        if (!parse_value_list(tokens, token_count, 1, values, &count)) {
            printf("usage: probe <value> [value ...]\n");
            return;
        }
        run_probe(ctx, values, count);
        return;
    }
    if (strcmp(tokens[0], "ddc-get") == 0 && token_count == 3) {
        handle_ddc_get(ctx, tokens[1], tokens[2]);
        return;
    }
    if (strcmp(tokens[0], "ddc-set") == 0 && token_count == 4) {
        handle_ddc_set(ctx, tokens[1], tokens[2], tokens[3]);
        return;
    }
    if (strcmp(tokens[0], "i2c-tx") == 0 && token_count >= 3) {
        handle_i2c_tx(ctx, tokens, token_count);
        return;
    }
    if (strcmp(tokens[0], "i2c-rx") == 0 && token_count == 3) {
        handle_i2c_rx(ctx, tokens[1], tokens[2]);
        return;
    }
    if (strcmp(tokens[0], "i2c-xfer") == 0 && token_count >= 4) {
        handle_i2c_xfer(ctx, tokens, token_count);
        return;
    }

    printf("unknown command; run 'help'\n");
}

static void debug_console_task(void *arg)
{
    debug_context_t *ctx = static_cast<debug_context_t *>(arg);
    char line[kCommandBufferSize] = {};
    size_t len = 0;

    print_prompt();
    while (true) {
        uint8_t ch = 0;
        int read = uart_read_bytes(kConsoleUart, &ch, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            line[len] = '\0';
            execute_command(ctx, line);
            len = 0;
            line[0] = '\0';
            print_prompt();
            continue;
        }

        if (ch == 0x08 || ch == 0x7f) {
            if (len > 0) {
                --len;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (!isprint(ch)) {
            continue;
        }

        if (len + 1 >= sizeof(line)) {
            printf("\ncommand too long\n");
            len = 0;
            line[0] = '\0';
            print_prompt();
            continue;
        }

        line[len++] = (char)ch;
        putchar((int)ch);
        fflush(stdout);
    }
}

extern "C" void app_main(void)
{
    static debug_context_t ctx = {};

    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_ERROR_CHECK(ddc_init(&ctx.ddc, 21, 22, 100000));
    ESP_ERROR_CHECK(uart_driver_install(kConsoleUart, kConsoleRxBuffer, 0, 0, NULL, 0));
    uart_flush_input(kConsoleUart);

    printf("\nDDC debug firmware\n");
    printf("Monitor control remains on the ESP even when the host switches away.\n");
    print_help();
    print_edid_info(&ctx);

    BaseType_t task_ok = xTaskCreate(debug_console_task, "debug_console", 6144, &ctx, 5, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}