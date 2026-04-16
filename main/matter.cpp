#include "matter.h"

#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <platform/CHIPDeviceEvent.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_cluster.h>
#include <esp_matter_endpoint.h>

#include "esp_log.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "matter_app";
static matter_callbacks_t g_callbacks = {};
static matter_runtime_t *g_runtime = nullptr;

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    (void)type;
    (void)endpoint_id;
    (void)effect_id;
    (void)effect_variant;
    (void)priv_data;
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    (void)priv_data;
    if (type != attribute::callback_type_t::PRE_UPDATE) {
        return ESP_OK;
    }

    if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        return g_callbacks.level_write ? g_callbacks.level_write(endpoint_id, val->val.u8, g_callbacks.ctx) : ESP_OK;
    }
    if (cluster_id == ModeSelect::Id && attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        return g_callbacks.mode_write ? g_callbacks.mode_write(val->val.u8, g_callbacks.ctx) : ESP_OK;
    }
    return ESP_OK;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address changed");
        break;
    default:
        break;
    }
}

extern "C" esp_err_t matter_start(const display_config_t *config, matter_runtime_t *runtime, const matter_callbacks_t *callbacks)
{
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb, nullptr);
    if (node == nullptr) {
        return ESP_FAIL;
    }

    g_callbacks = *callbacks;
    g_runtime = runtime;
    std::strncpy(g_runtime->device_name, config->monitor_name, sizeof(g_runtime->device_name) - 1);

    endpoint::dimmable_light::config_t brightness_cfg;
    brightness_cfg.on_off.on_off = true;
    brightness_cfg.level_control.current_level = 128;
    endpoint_t *brightness = endpoint::dimmable_light::create(node, &brightness_cfg, ENDPOINT_FLAG_NONE, nullptr);
    endpoint::dimmable_light::config_t contrast_cfg;
    contrast_cfg.on_off.on_off = true;
    contrast_cfg.level_control.current_level = 128;
    endpoint_t *contrast = endpoint::dimmable_light::create(node, &contrast_cfg, ENDPOINT_FLAG_NONE, nullptr);

    endpoint_t *input = endpoint::create(node, ENDPOINT_FLAG_NONE);
    cluster::descriptor::config_t descriptor_cfg;
    cluster::identify::config_t identify_cfg;
    cluster::descriptor::create(input, &descriptor_cfg, CLUSTER_FLAG_SERVER);
    cluster::identify::create(input, &identify_cfg, CLUSTER_FLAG_SERVER);
    cluster::mode_select::config_t mode_cfg;
    mode_cfg.current_mode = 0;
    cluster_t *mode_cluster = cluster::mode_select::create(input, &mode_cfg, CLUSTER_FLAG_SERVER);
    (void)mode_cluster;

    runtime->brightness_endpoint_id = endpoint::get_id(brightness);
    runtime->contrast_endpoint_id = endpoint::get_id(contrast);
    runtime->input_endpoint_id = endpoint::get_id(input);

    return esp_matter::start(app_event_cb);
}

extern "C" esp_err_t matter_update_level(uint16_t endpoint_id, uint8_t level)
{
    esp_matter_attr_val_t val = esp_matter_uint8(level);
    return attribute::update(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);
}

extern "C" esp_err_t matter_update_mode(uint16_t endpoint_id, uint8_t mode)
{
    esp_matter_attr_val_t val = esp_matter_uint8(mode);
    return attribute::update(endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &val);
}
