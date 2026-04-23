#include "webserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "webserver";
static const size_t HTTPD_STACK_SIZE = 4096;
static const uint16_t HTTPD_MAX_OPEN_SOCKETS = 2;
static const uint16_t HTTPD_MAX_RESP_HEADERS = 4;
static const uint16_t HTTPD_BACKLOG_CONN = 1;
static webserver_context_t *s_ctx;

typedef struct {
    uint8_t brightness_vcp;
    uint8_t contrast_vcp;
} level_request_t;

static bool is_standard_level_vcp(bool contrast, uint8_t vcp)
{
    return contrast ? (vcp == 0x12) : (vcp == 0x10);
}

extern const uint8_t frontend_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t frontend_index_html_end[] asm("_binary_index_html_end");

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static cJSON *build_config_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "pnpId", s_ctx->config->pnp_id);
    cJSON_AddStringToObject(root, "monitorName", s_ctx->config->monitor_name);
    cJSON_AddBoolToObject(root, "dbMatch", s_ctx->config->db_match);
    cJSON_AddBoolToObject(root, "profileCached", s_ctx->config->profile_cached);
    cJSON_AddBoolToObject(root, "userOverride", s_ctx->config->user_override);
    cJSON_AddNumberToObject(root, "brightnessVcp", s_ctx->config->brightness_vcp);
    cJSON_AddNumberToObject(root, "contrastVcp", s_ctx->config->contrast_vcp);
    cJSON_AddStringToObject(root, "profileUrl", s_ctx->profile->url);

    cJSON *inputs = cJSON_AddArrayToObject(root, "inputs");
    for (size_t i = 0; i < INPUT_SLOT_COUNT; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "slot", i);
        cJSON_AddNumberToObject(item, "value", s_ctx->config->inputs[i].value);
        cJSON_AddBoolToObject(item, "enabled", s_ctx->config->inputs[i].enabled);
        cJSON_AddStringToObject(item, "name", s_ctx->config->inputs[i].name);
        cJSON_AddItemToArray(inputs, item);
    }
    return root;
}

static void init_level_request(level_request_t *request)
{
    request->brightness_vcp = s_ctx->config->brightness_vcp;
    request->contrast_vcp = s_ctx->config->contrast_vcp;
}

static void parse_level_query(httpd_req_t *req, level_request_t *request)
{
    char query[96] = {0};
    if (httpd_req_get_url_query_len(req) <= 0) {
        return;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }

    char value[8] = {0};
    if (httpd_query_key_value(query, "brightnessVcp", value, sizeof(value)) == ESP_OK) {
        request->brightness_vcp = (uint8_t)strtoul(value, NULL, 0);
    }
    if (httpd_query_key_value(query, "contrastVcp", value, sizeof(value)) == ESP_OK) {
        request->contrast_vcp = (uint8_t)strtoul(value, NULL, 0);
    }
}

static void parse_level_json(cJSON *json, level_request_t *request)
{
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(json, "brightnessVcp");
    cJSON *contrast = cJSON_GetObjectItemCaseSensitive(json, "contrastVcp");
    if (cJSON_IsNumber(brightness)) {
        request->brightness_vcp = (uint8_t)brightness->valueint;
    }
    if (cJSON_IsNumber(contrast)) {
        request->contrast_vcp = (uint8_t)contrast->valueint;
    }
}

static cJSON *build_levels_json(const level_request_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    const struct {
        const char *name;
        bool contrast;
    } level_defs[] = {
        {"brightness", false},
        {"contrast", true},
    };

    for (size_t index = 0; index < sizeof(level_defs) / sizeof(level_defs[0]); ++index) {
        uint8_t vcp = level_defs[index].contrast ? request->contrast_vcp : request->brightness_vcp;
        ddc_vcp_value_t value = {};
        esp_err_t err = s_ctx->get_level ? s_ctx->get_level(level_defs[index].contrast, vcp, &value, s_ctx->ctx) : ESP_FAIL;
        bool write_only = (err != ESP_OK || !value.present) && is_standard_level_vcp(level_defs[index].contrast, vcp);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "vcp", vcp);
        cJSON_AddBoolToObject(item, "present", err == ESP_OK && value.present);
        cJSON_AddBoolToObject(item, "writeOnly", write_only);
        cJSON_AddNumberToObject(item, "current", value.current_value);
        cJSON_AddNumberToObject(item, "maximum", value.maximum_value > 0 ? value.maximum_value : (write_only ? 100 : 0));
        cJSON_AddItemToObject(root, level_defs[index].name, item);
    }

    return root;
}

static cJSON *build_input_source_json(void)
{
    web_input_source_state_t input_state = {};
    esp_err_t err = s_ctx->get_input_source_state ? s_ctx->get_input_source_state(&input_state, s_ctx->ctx) : ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);

    const struct {
        const char *name;
        const ddc_vcp_value_t *value;
        uint8_t vcp;
    } defs[] = {
        {"standard", &input_state.standard, 0x60},
        {"alternate", &input_state.alternate, 0xF4},
        {"resolved", &input_state.resolved, 0x00},
    };

    for (size_t index = 0; index < sizeof(defs) / sizeof(defs[0]); ++index) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject(item, "present", defs[index].value->present);
        cJSON_AddNumberToObject(item, "current", defs[index].value->current_value);
        cJSON_AddNumberToObject(item, "maximum", defs[index].value->maximum_value);
        cJSON_AddNumberToObject(item, "vcp", defs[index].vcp);
        cJSON_AddItemToObject(root, defs[index].name, item);
    }

    cJSON_AddNumberToObject(root, "matchedSlot", input_state.matched_slot);
    cJSON_AddStringToObject(root, "matchedName", input_state.matched_name);
    return root;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)frontend_index_html_start,
                           frontend_index_html_end - frontend_index_html_start);
}

static esp_err_t get_config_handler(httpd_req_t *req)
{
    cJSON *json = build_config_json();
    if (json == NULL) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t get_levels_handler(httpd_req_t *req)
{
    level_request_t request = {};
    init_level_request(&request);
    parse_level_query(req, &request);

    cJSON *json = build_levels_json(&request);
    if (json == NULL) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t get_input_source_handler(httpd_req_t *req)
{
    cJSON *json = build_input_source_json();
    if (json == NULL) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    esp_err_t err = send_json(req, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t save_config_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing body\"}");
    }

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    display_config_t updated = *s_ctx->config;
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(json, "brightnessVcp");
    cJSON *contrast = cJSON_GetObjectItemCaseSensitive(json, "contrastVcp");
    cJSON *inputs = cJSON_GetObjectItemCaseSensitive(json, "inputs");
    if (cJSON_IsNumber(brightness)) {
        updated.brightness_vcp = (uint8_t)brightness->valueint;
    }
    if (cJSON_IsNumber(contrast)) {
        updated.contrast_vcp = (uint8_t)contrast->valueint;
    }
    if (cJSON_IsArray(inputs)) {
        size_t i = 0;
        cJSON *input = NULL;
        cJSON_ArrayForEach(input, inputs) {
            if (i >= INPUT_SLOT_COUNT) {
                break;
            }
            cJSON *value = cJSON_GetObjectItemCaseSensitive(input, "value");
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(input, "name");
            if (cJSON_IsNumber(value)) {
                updated.inputs[i].value = (uint8_t)value->valueint;
            }
            if (cJSON_IsBool(enabled)) {
                updated.inputs[i].enabled = cJSON_IsTrue(enabled);
            }
            if (cJSON_IsString(name) && name->valuestring != NULL) {
                strncpy(updated.inputs[i].name, name->valuestring, sizeof(updated.inputs[i].name) - 1);
                updated.inputs[i].name[sizeof(updated.inputs[i].name) - 1] = '\0';
            }
            ++i;
        }
    }
    updated.user_override = true;

    cJSON_Delete(json);
    if (s_ctx->apply_config(&updated, s_ctx->ctx) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"save failed\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t test_input_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing body\"}");
    }
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid json\"}");
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
    esp_err_t err = cJSON_IsNumber(value) ? s_ctx->test_input((uint8_t)value->valueint, s_ctx->ctx) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"test failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t probe_inputs_handler(httpd_req_t *req)
{
    esp_err_t err = s_ctx->probe_inputs ? s_ctx->probe_inputs(s_ctx->ctx) : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"probe failed\"}");
    }
    return get_config_handler(req);
}

static esp_err_t set_level_handler(httpd_req_t *req)
{
    char body[160] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing body\"}");
    }

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return send_error(req, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    cJSON *kind = cJSON_GetObjectItemCaseSensitive(json, "kind");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
    level_request_t request = {};
    init_level_request(&request);
    parse_level_json(json, &request);
    bool contrast = cJSON_IsString(kind) && kind->valuestring != NULL && strcmp(kind->valuestring, "contrast") == 0;
    bool brightness = cJSON_IsString(kind) && kind->valuestring != NULL && strcmp(kind->valuestring, "brightness") == 0;
    uint8_t vcp = contrast ? request.contrast_vcp : request.brightness_vcp;
    esp_err_t err = (brightness || contrast) && cJSON_IsNumber(value) && s_ctx->set_level ?
        s_ctx->set_level(contrast, vcp, (uint8_t)value->valueint, s_ctx->ctx) :
        ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);

    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"level write failed\"}");
    }

    cJSON *response = build_levels_json(&request);
    if (response == NULL) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    err = send_json(req, response);
    cJSON_Delete(response);
    return err;
}

static esp_err_t detect_handler(httpd_req_t *req)
{
    char query[64] = {0};
    bool refresh = false;
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[8] = {0};
        if (httpd_query_key_value(query, "refresh_db", value, sizeof(value)) == ESP_OK && strcmp(value, "1") == 0) {
            refresh = true;
        }
    }
    esp_err_t err = refresh ? s_ctx->refresh_db(s_ctx->ctx) : s_ctx->detect(s_ctx->ctx);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"detect failed\"}");
    }
    return get_config_handler(req);
}

static esp_err_t open_commissioning_window_handler(httpd_req_t *req)
{
    esp_err_t err = s_ctx->open_commissioning_window ? s_ctx->open_commissioning_window(s_ctx->ctx) : ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_ERR_TIMEOUT) {
        return send_error(req, "504 Gateway Timeout",
                          "{\"ok\":false,\"error\":\"commissioning window request timed out\"}");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error",
                          "{\"ok\":false,\"error\":\"commissioning window failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"timeoutSeconds\":900}");
}

esp_err_t webserver_start(webserver_context_t *ctx)
{
    s_ctx = ctx;

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = HTTPD_STACK_SIZE;
    config.max_open_sockets = HTTPD_MAX_OPEN_SOCKETS;
    config.max_uri_handlers = 10;
    config.max_resp_headers = HTTPD_MAX_RESP_HEADERS;
    config.backlog_conn = HTTPD_BACKLOG_CONN;
    ESP_LOGI(TAG, "starting httpd free_heap=%u internal_heap=%u", (unsigned int)esp_get_free_heap_size(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t get_config = {.uri = "/api/config", .method = HTTP_GET, .handler = get_config_handler};
    httpd_uri_t get_levels = {.uri = "/api/levels", .method = HTTP_GET, .handler = get_levels_handler};
    httpd_uri_t get_input_source = {.uri = "/api/input-source", .method = HTTP_GET, .handler = get_input_source_handler};
    httpd_uri_t save_config = {.uri = "/api/config", .method = HTTP_POST, .handler = save_config_handler};
    httpd_uri_t test_input = {.uri = "/api/test", .method = HTTP_POST, .handler = test_input_handler};
    httpd_uri_t probe_inputs = {.uri = "/api/probe-inputs", .method = HTTP_POST, .handler = probe_inputs_handler};
    httpd_uri_t set_level = {.uri = "/api/levels", .method = HTTP_POST, .handler = set_level_handler};
    httpd_uri_t detect = {.uri = "/api/detect", .method = HTTP_GET, .handler = detect_handler};
    httpd_uri_t open_commissioning_window = {
        .uri = "/api/matter/open-commissioning-window",
        .method = HTTP_POST,
        .handler = open_commissioning_window_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &index), TAG, "index register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_config), TAG, "config get register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_levels), TAG, "levels get register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_input_source), TAG, "input source get register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save_config), TAG, "config post register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &test_input), TAG, "test register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &probe_inputs), TAG, "probe register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &set_level), TAG, "levels post register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &detect), TAG, "detect register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &open_commissioning_window), TAG,
                        "commissioning window register failed");
    ESP_LOGI(TAG, "web ui HTTP server started on port 80");
    return ESP_OK;
}
