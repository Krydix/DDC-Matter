#include "webserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "webserver";
static webserver_context_t *s_ctx;

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
        cJSON_AddStringToObject(item, "name", s_ctx->config->inputs[i].name);
        cJSON_AddItemToArray(inputs, item);
    }
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
            cJSON *name = cJSON_GetObjectItemCaseSensitive(input, "name");
            if (cJSON_IsNumber(value)) {
                updated.inputs[i].value = (uint8_t)value->valueint;
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

esp_err_t webserver_start(webserver_context_t *ctx)
{
    s_ctx = ctx;

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set("display-switcher"), TAG, "hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("ESP32 Display Switcher"), TAG, "instance failed");
    ESP_RETURN_ON_ERROR(mdns_service_add("Display Switcher", "_http", "_tcp", 80, NULL, 0), TAG, "service failed");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd start failed");

    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t get_config = {.uri = "/api/config", .method = HTTP_GET, .handler = get_config_handler};
    httpd_uri_t save_config = {.uri = "/api/config", .method = HTTP_POST, .handler = save_config_handler};
    httpd_uri_t test_input = {.uri = "/api/test", .method = HTTP_POST, .handler = test_input_handler};
    httpd_uri_t detect = {.uri = "/api/detect", .method = HTTP_GET, .handler = detect_handler};

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &index), TAG, "index register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_config), TAG, "config get register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save_config), TAG, "config post register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &test_input), TAG, "test register failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &detect), TAG, "detect register failed");
    ESP_LOGI(TAG, "web ui available at http://display-switcher.local/");
    return ESP_OK;
}
