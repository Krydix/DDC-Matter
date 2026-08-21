#include "matter.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>
#include <platform/CHIPDeviceEvent.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_cluster.h>
#include <esp_matter_endpoint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "matter_app";
static constexpr char kAccessoryName[] = "Display-Switcher";
static constexpr char kBrightnessName[] = "Brightness";
static constexpr char kContrastName[] = "Contrast";
static constexpr char kInputPrefix[] = "Input";
static constexpr uint32_t kCommissioningWindowTimeoutSecs = 15 * 60;
static constexpr TickType_t kInputResetDelay = pdMS_TO_TICKS(1000);
static constexpr size_t kBrightnessEndpointIndex = 0;
static constexpr size_t kContrastEndpointIndex = 1;
static constexpr size_t kInputEndpointIndexBase = 2;
static constexpr size_t kBridgedEndpointCount = kInputEndpointIndexBase + INPUT_SLOT_COUNT;

static const char *input_label_for_slot(const display_config_t *config, size_t index);

static uint8_t ddc_level_to_matter_level(uint8_t ddc_level)
{
    uint8_t clamped = std::min<uint8_t>(ddc_level, 100);
    return static_cast<uint8_t>((static_cast<uint16_t>(clamped) * 254U + 50U) / 100U);
}

struct InputResetRequest {
    uint16_t endpoint_id;
};

static matter_callbacks_t g_callbacks = {};
static matter_runtime_t *g_runtime = nullptr;
static bool g_internal_attribute_update = false;
static std::array<std::array<char, INPUT_NAME_MAX_LEN>, INPUT_SLOT_COUNT> g_default_input_labels = {};
static node_t *g_node = nullptr;
static endpoint_t *g_aggregator_endpoint = nullptr;
static std::array<endpoint::bridged_node::config_t, kBridgedEndpointCount> g_bridged_node_configs = {};
static std::array<
    std::array<char, cluster::bridged_device_basic_information::k_max_unique_id_length + 1>, kBridgedEndpointCount>
    g_bridged_unique_ids = {};
static endpoint::dimmable_light::config_t g_brightness_endpoint_config = {};
static endpoint::dimmable_light::config_t g_contrast_endpoint_config = {};
static std::array<endpoint::on_off_light::config_t, INPUT_SLOT_COUNT> g_input_endpoint_configs = {};
static std::array<endpoint_t *, INPUT_SLOT_COUNT> g_input_endpoints = {};

class MatterStackLockGuard {
public:
    MatterStackLockGuard() : status_(lock::chip_stack_lock(portMAX_DELAY)) {}

    ~MatterStackLockGuard()
    {
        if (status_ == lock::SUCCESS) {
            lock::chip_stack_unlock();
        }
    }

    bool acquired() const
    {
        return status_ != lock::FAILED;
    }

private:
    lock::status_t status_;
};

static bool is_input_endpoint(uint16_t endpoint_id)
{
    if (g_runtime == nullptr) {
        return false;
    }

    for (uint16_t input_endpoint_id : g_runtime->input_endpoint_ids) {
        if (input_endpoint_id == endpoint_id) {
            return true;
        }
    }

    return false;
}

static void prepare_bridged_node_config(size_t index, const char *unique_id)
{
    g_bridged_node_configs[index] = {};
    std::snprintf(g_bridged_unique_ids[index].data(), g_bridged_unique_ids[index].size(), "%s", unique_id);
}

static esp_err_t add_bridged_node_metadata(endpoint_t *endpoint, const char *label, const char *unique_id)
{
    cluster_t *cluster = cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
    VerifyOrReturnValue(cluster != nullptr, ESP_ERR_NOT_FOUND,
                        ESP_LOGE(TAG, "Bridged Device Basic Information cluster unavailable"));

    // This esp-matter release loses a restored nonvolatile string's maximum capacity,
    // causing later NodeLabel writes to fail with ESP_ERR_NO_MEM. The web configuration
    // is already persistent, so recreate this writable attribute from that config at boot.
    VerifyOrReturnValue(
        attribute::create(cluster, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, ATTRIBUTE_FLAG_WRITABLE,
                          esp_matter_char_str(const_cast<char *>(label), std::strlen(label)),
                          cluster::bridged_device_basic_information::k_max_node_label_length) != nullptr,
        ESP_FAIL, ESP_LOGE(TAG, "Failed to create bridged NodeLabel"));

    // The cluster creates an empty UniqueID attribute. Set its value instead of trying
    // to create the same attribute a second time.
    attribute_t *unique_id_attribute =
        attribute::get(cluster, BridgedDeviceBasicInformation::Attributes::UniqueID::Id);
    VerifyOrReturnValue(unique_id_attribute != nullptr, ESP_ERR_NOT_FOUND,
                        ESP_LOGE(TAG, "Bridged UniqueID attribute unavailable"));
    esp_matter_attr_val_t unique_id_value =
        esp_matter_char_str(const_cast<char *>(unique_id), std::strlen(unique_id));
    return attribute::set_val(unique_id_attribute, &unique_id_value);
}

static esp_err_t update_bridged_node_label(uint16_t endpoint_id, const char *label)
{
    esp_matter_attr_val_t value = esp_matter_char_str(const_cast<char *>(label), std::strlen(label));
    return attribute::update(endpoint_id, BridgedDeviceBasicInformation::Id,
                             BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &value);
}

static endpoint_t *create_bridged_endpoint(size_t bridged_index, const char *label, bool dimmable, size_t input_index)
{
    endpoint_t *endpoint = endpoint::bridged_node::create(
        g_node, &g_bridged_node_configs[bridged_index], ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE, nullptr);
    VerifyOrReturnValue(endpoint != nullptr, nullptr, ESP_LOGE(TAG, "Failed to create bridged node endpoint"));

    esp_err_t err = dimmable
        ? endpoint::dimmable_light::add(endpoint, bridged_index == kBrightnessEndpointIndex
                                                      ? &g_brightness_endpoint_config
                                                      : &g_contrast_endpoint_config)
        : endpoint::on_off_light::add(endpoint, &g_input_endpoint_configs[input_index]);
    VerifyOrReturnValue(err == ESP_OK, nullptr, ESP_LOGE(TAG, "Failed to add bridged application device type"));
    VerifyOrReturnValue(endpoint::set_parent_endpoint(endpoint, g_aggregator_endpoint) == ESP_OK, nullptr,
                        ESP_LOGE(TAG, "Failed to parent bridged endpoint to aggregator"));
    VerifyOrReturnValue(add_bridged_node_metadata(endpoint, label, g_bridged_unique_ids[bridged_index].data()) == ESP_OK,
                        nullptr, ESP_LOGE(TAG, "Failed to add bridged endpoint metadata"));
    return endpoint;
}

static esp_err_t resume_input_endpoint(size_t index, const char *label)
{
    VerifyOrReturnValue(g_input_endpoints[index] == nullptr, ESP_ERR_INVALID_STATE,
                        ESP_LOGE(TAG, "input endpoint %u is already active", static_cast<unsigned int>(index)));

    const uint16_t endpoint_id = g_runtime->input_endpoint_ids[index];
    const size_t bridged_index = kInputEndpointIndexBase + index;
    endpoint_t *endpoint = endpoint::bridged_node::resume(
        g_node, &g_bridged_node_configs[bridged_index], ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE, endpoint_id,
        nullptr);
    VerifyOrReturnValue(endpoint != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "failed to resume input endpoint %u", endpoint_id));

    esp_err_t err = endpoint::on_off_light::add(endpoint, &g_input_endpoint_configs[index]);
    if (err == ESP_OK) {
        err = endpoint::set_parent_endpoint(endpoint, g_aggregator_endpoint);
    }
    if (err == ESP_OK) {
        err = add_bridged_node_metadata(endpoint, label, g_bridged_unique_ids[bridged_index].data());
    }
    if (err == ESP_OK) {
        err = endpoint::enable(endpoint);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to rebuild input endpoint %u: %s", endpoint_id, esp_err_to_name(err));
        endpoint::destroy(g_node, endpoint);
        return err;
    }

    g_input_endpoints[index] = endpoint;
    ESP_RETURN_ON_ERROR(matter_update_input_state(endpoint_id, false), TAG, "input state reset failed");
    ESP_LOGI(TAG, "resumed input endpoint %u (%s)", endpoint_id, label);
    return ESP_OK;
}

static esp_err_t destroy_input_endpoint(size_t index)
{
    endpoint_t *endpoint = g_input_endpoints[index];
    if (endpoint == nullptr) {
        return ESP_OK;
    }

    const uint16_t endpoint_id = endpoint::get_id(endpoint);
    ESP_RETURN_ON_ERROR(matter_update_input_state(endpoint_id, false), TAG, "input state reset failed");
    ESP_RETURN_ON_ERROR(endpoint::destroy(g_node, endpoint), TAG, "input endpoint destroy failed");
    g_input_endpoints[index] = nullptr;
    ESP_LOGI(TAG, "removed input endpoint %u", endpoint_id);
    return ESP_OK;
}

class InternalAttributeUpdateGuard {
public:
    InternalAttributeUpdateGuard()
    {
        g_internal_attribute_update = true;
    }

    ~InternalAttributeUpdateGuard()
    {
        g_internal_attribute_update = false;
    }
};

static const char *input_label_for_slot(const display_config_t *config, size_t index)
{
    const char *configured = config->inputs[index].name;
    if (configured[0] != '\0') {
        return configured;
    }

    std::snprintf(g_default_input_labels[index].data(), g_default_input_labels[index].size(), "%s %u", kInputPrefix,
                  static_cast<unsigned int>(index + 1));
    return g_default_input_labels[index].data();
}

static void reset_input_endpoint_task(void *arg)
{
    std::unique_ptr<InputResetRequest> request(static_cast<InputResetRequest *>(arg));
    vTaskDelay(kInputResetDelay);
    matter_update_input_state(request->endpoint_id, false);
    vTaskDelete(NULL);
}

static void schedule_input_reset(uint16_t endpoint_id)
{
    InputResetRequest *request = new InputResetRequest{endpoint_id};
    if (request == nullptr) {
        ESP_LOGW(TAG, "failed to allocate input reset request");
        return;
    }

    BaseType_t task_ok = xTaskCreate(reset_input_endpoint_task, "matter_input_reset", 3072, request, 5, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "failed to create input reset task");
        delete request;
    }
}

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
    if (g_internal_attribute_update) {
        return ESP_OK;
    }

    if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        return g_callbacks.level_write ? g_callbacks.level_write(endpoint_id, val->val.u8, g_callbacks.ctx) : ESP_OK;
    }
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id && is_input_endpoint(endpoint_id)) {
        if (!val->val.b) {
            return ESP_OK;
        }

        esp_err_t err = g_callbacks.input_write ? g_callbacks.input_write(endpoint_id, val->val.b, g_callbacks.ctx) : ESP_OK;
        if (err == ESP_OK) {
            schedule_input_reset(endpoint_id);
        }
        return err;
    }
    return ESP_OK;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    static bool s_commissioning_complete_fired = false;
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        if (!s_commissioning_complete_fired && g_callbacks.commissioning_complete) {
            s_commissioning_complete_fired = true;
            g_callbacks.commissioning_complete(g_callbacks.ctx);
        }
        break;
    default:
        break;
    }
}

extern "C" esp_err_t matter_start(const display_config_t *config, matter_runtime_t *runtime, const matter_callbacks_t *callbacks)
{
    node::config_t node_config;
    std::strncpy(node_config.root_node.basic_information.node_label, kAccessoryName,
                 sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_config.root_node.basic_information.node_label[
        sizeof(node_config.root_node.basic_information.node_label) - 1] = '\0';
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb, nullptr);
    if (node == nullptr) {
        return ESP_FAIL;
    }

    g_node = node;
    g_callbacks = *callbacks;
    g_runtime = runtime;
    std::strncpy(g_runtime->device_name, config->monitor_name, sizeof(g_runtime->device_name) - 1);

    endpoint::aggregator::config_t aggregator_config;
    g_aggregator_endpoint = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    VerifyOrReturnValue(g_aggregator_endpoint != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to create Matter bridge aggregator"));

    prepare_bridged_node_config(kBrightnessEndpointIndex, "ddc-brightness");
    prepare_bridged_node_config(kContrastEndpointIndex, "ddc-contrast");
    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        char unique_id[cluster::bridged_device_basic_information::k_max_unique_id_length + 1] = {};
        std::snprintf(unique_id, sizeof(unique_id), "ddc-input-%u", static_cast<unsigned int>(index + 1));
        prepare_bridged_node_config(kInputEndpointIndexBase + index, unique_id);
    }

    g_brightness_endpoint_config = {};
    g_brightness_endpoint_config.on_off.on_off = true;
    g_brightness_endpoint_config.level_control.current_level = 128;
    endpoint_t *brightness = create_bridged_endpoint(kBrightnessEndpointIndex, kBrightnessName, true, 0);
    VerifyOrReturnValue(brightness != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create brightness endpoint"));

    g_contrast_endpoint_config = {};
    g_contrast_endpoint_config.on_off.on_off = true;
    g_contrast_endpoint_config.level_control.current_level = 128;
    endpoint_t *contrast = create_bridged_endpoint(kContrastEndpointIndex, kContrastName, true, 0);
    VerifyOrReturnValue(contrast != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create contrast endpoint"));

    runtime->brightness_endpoint_id = endpoint::get_id(brightness);
    runtime->contrast_endpoint_id = endpoint::get_id(contrast);

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        g_input_endpoint_configs[index] = {};
        g_input_endpoint_configs[index].on_off.on_off = false;
        endpoint_t *input = create_bridged_endpoint(kInputEndpointIndexBase + index, input_label_for_slot(config, index),
                                                    false, index);
        VerifyOrReturnValue(input != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create input endpoint %u",
                                                                 static_cast<unsigned int>(index)));
        g_input_endpoints[index] = input;
        runtime->input_endpoint_ids[index] = endpoint::get_id(input);
    }

    ESP_RETURN_ON_ERROR(esp_matter::start(app_event_cb), TAG, "esp_matter start failed");
    esp_err_t sync_err = matter_sync_input_endpoints(config);
    if (sync_err != ESP_OK) {
        ESP_LOGW(TAG, "initial input endpoint sync deferred: %s", esp_err_to_name(sync_err));
    }

    return ESP_OK;
}

extern "C" esp_err_t matter_update_level(uint16_t endpoint_id, uint8_t level)
{
    InternalAttributeUpdateGuard guard;
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(ddc_level_to_matter_level(level));
    return attribute::update(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);
}

extern "C" esp_err_t matter_update_input_state(uint16_t endpoint_id, bool on)
{
    InternalAttributeUpdateGuard guard;
    esp_matter_attr_val_t val = esp_matter_bool(on);
    return attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

extern "C" esp_err_t matter_sync_input_endpoints(const display_config_t *config)
{
    VerifyOrReturnValue(g_runtime != nullptr, ESP_ERR_INVALID_STATE, ESP_LOGE(TAG, "matter runtime unavailable"));
    MatterStackLockGuard stack_lock;
    VerifyOrReturnValue(stack_lock.acquired(), ESP_FAIL, ESP_LOGE(TAG, "failed to lock Matter stack"));

    // Remove disabled bridge children before resuming or updating enabled ones.
    // esp-matter requires destroy/resume for dynamic endpoints in this pinned release;
    // keeping the original endpoint ID preserves the logical accessory identity.
    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        if (!config->inputs[index].enabled && g_input_endpoints[index] != nullptr) {
            ESP_RETURN_ON_ERROR(destroy_input_endpoint(index), TAG, "input endpoint removal failed");
        }
    }

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        uint16_t endpoint_id = g_runtime->input_endpoint_ids[index];
        const char *label = input_label_for_slot(config, index);

        if (!config->inputs[index].enabled) {
            continue;
        }
        if (g_input_endpoints[index] == nullptr) {
            ESP_RETURN_ON_ERROR(resume_input_endpoint(index, label), TAG, "input endpoint resume failed");
            continue;
        }
        ESP_RETURN_ON_ERROR(update_bridged_node_label(endpoint_id, label), TAG, "input bridged name update failed");
    }

    return ESP_OK;
}

extern "C" esp_err_t matter_open_basic_commissioning_window(void)
{
    MatterStackLockGuard stack_lock;
    VerifyOrReturnValue(stack_lock.acquired(), ESP_FAIL, ESP_LOGE(TAG, "failed to lock Matter stack"));
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(kCommissioningWindowTimeoutSecs));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "failed to open basic commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "basic commissioning window opened for %lu seconds",
             static_cast<unsigned long>(kCommissioningWindowTimeoutSecs));
    return ESP_OK;
}

extern "C" bool matter_is_commissioned(void)
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}
