#include "matter.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <platform/DeviceInfoProvider.h>
#include <platform/PlatformManager.h>
#include <platform/CHIPDeviceEvent.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_cluster.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_providers.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "matter_app";
static constexpr char kAccessoryName[] = "Display-Switcher";
static constexpr char kLabelKey[] = "name";
static constexpr char kBrightnessName[] = "Brightness";
static constexpr char kContrastName[] = "Contrast";
static constexpr char kInputPrefix[] = "Input";
static constexpr uint32_t kCommissioningWindowTimeoutSecs = 15 * 60;
static constexpr TickType_t kInputResetDelay = pdMS_TO_TICKS(1000);
static constexpr size_t kManagedEndpointCount = 2 + INPUT_SLOT_COUNT;
static constexpr uint16_t kRootEndpointId = 0;

static const char *input_label_for_slot(const display_config_t *config, size_t index);

static esp_err_t sync_root_basic_information_metadata(const char *name);

static uint8_t ddc_level_to_matter_level(uint8_t ddc_level)
{
    uint8_t clamped = std::min<uint8_t>(ddc_level, 100);
    return static_cast<uint8_t>((static_cast<uint16_t>(clamped) * 254U + 50U) / 100U);
}

struct CommissioningWindowRequest {
    TaskHandle_t waiting_task;
    esp_err_t result;
};

struct InputResetRequest {
    uint16_t endpoint_id;
};

struct FixedLabelEntry {
    chip::EndpointId endpoint_id;
    chip::CharSpan label;
    chip::CharSpan value;
};

struct UserLabelStorage {
    char label[chip::DeviceLayer::kMaxLabelNameLength + 1] = {};
    char value[chip::DeviceLayer::kMaxLabelValueLength + 1] = {};
};

struct EndpointUserLabelState {
    chip::EndpointId endpoint_id = chip::kInvalidEndpointId;
    size_t length = 0;
    std::array<UserLabelStorage, chip::DeviceLayer::kMaxUserLabelListLength> labels = {};
};

static void open_commissioning_window_work(intptr_t arg)
{
    auto *request = reinterpret_cast<CommissioningWindowRequest *>(arg);
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(kCommissioningWindowTimeoutSecs));
    request->result = (err == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    if (request->result == ESP_OK) {
        ESP_LOGI(TAG, "basic commissioning window opened for %lu seconds",
                 static_cast<unsigned long>(kCommissioningWindowTimeoutSecs));
    } else {
        ESP_LOGW(TAG, "failed to open basic commissioning window");
    }
    xTaskNotifyGive(request->waiting_task);
}

class DeviceInfoProviderImpl : public chip::DeviceLayer::DeviceInfoProvider {
public:
    class FixedLabelIteratorImpl : public chip::DeviceLayer::DeviceInfoProvider::FixedLabelIterator {
    public:
        FixedLabelIteratorImpl(chip::EndpointId endpoint_id, const FixedLabelEntry *entries, size_t entry_count) :
            m_endpoint_id(endpoint_id), m_entries(entries), m_entry_count(entry_count)
        {}

        size_t Count() override
        {
            size_t count = 0;
            for (size_t index = 0; index < m_entry_count; ++index) {
                if (m_entries[index].endpoint_id == m_endpoint_id) {
                    ++count;
                }
            }
            return count;
        }

        bool Next(chip::DeviceLayer::DeviceInfoProvider::FixedLabelType &output) override
        {
            while (m_index < m_entry_count) {
                const FixedLabelEntry &entry = m_entries[m_index++];
                if (entry.endpoint_id == m_endpoint_id) {
                    output.label = entry.label;
                    output.value = entry.value;
                    return true;
                }
            }
            return false;
        }

        void Release() override
        {
            chip::Platform::Delete(this);
        }

    private:
        chip::EndpointId m_endpoint_id;
        const FixedLabelEntry *m_entries;
        size_t m_entry_count;
        size_t m_index = 0;
    };

    class UserLabelIteratorImpl : public chip::DeviceLayer::DeviceInfoProvider::UserLabelIterator {
    public:
        explicit UserLabelIteratorImpl(const EndpointUserLabelState *state) : m_state(state) {}

        size_t Count() override
        {
            return m_state ? m_state->length : 0;
        }

        bool Next(chip::DeviceLayer::DeviceInfoProvider::UserLabelType &output) override
        {
            if (!m_state || m_index >= m_state->length) {
                return false;
            }

            const UserLabelStorage &label = m_state->labels[m_index++];
            output.label = chip::CharSpan::fromCharString(label.label);
            output.value = chip::CharSpan::fromCharString(label.value);
            return true;
        }

        void Release() override
        {
            chip::Platform::Delete(this);
        }

    private:
        const EndpointUserLabelState *m_state;
        size_t m_index = 0;
    };

    void SetFixedLabelEntries(const FixedLabelEntry *entries, size_t entry_count)
    {
        m_entries = entries;
        m_entry_count = entry_count;
    }

    chip::DeviceLayer::DeviceInfoProvider::FixedLabelIterator *IterateFixedLabel(chip::EndpointId endpoint) override
    {
        return chip::Platform::New<FixedLabelIteratorImpl>(endpoint, m_entries, m_entry_count);
    }

    chip::DeviceLayer::DeviceInfoProvider::UserLabelIterator *IterateUserLabel(chip::EndpointId endpoint) override
    {
        return chip::Platform::New<UserLabelIteratorImpl>(FindOrNull(endpoint));
    }

    chip::DeviceLayer::DeviceInfoProvider::SupportedLocalesIterator *IterateSupportedLocales() override
    {
        return nullptr;
    }

    chip::DeviceLayer::DeviceInfoProvider::SupportedCalendarTypesIterator *IterateSupportedCalendarTypes() override
    {
        return nullptr;
    }

protected:
    CHIP_ERROR SetUserLabelAt(chip::EndpointId endpoint, size_t index,
                              const chip::DeviceLayer::DeviceInfoProvider::UserLabelType &user_label) override
    {
        VerifyOrReturnValue(index < chip::DeviceLayer::kMaxUserLabelListLength, CHIP_ERROR_INVALID_ARGUMENT);

        EndpointUserLabelState &state = FindOrCreate(endpoint);
        CopySpan(user_label.label, state.labels[index].label, sizeof(state.labels[index].label));
        CopySpan(user_label.value, state.labels[index].value, sizeof(state.labels[index].value));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId endpoint, size_t index) override
    {
        EndpointUserLabelState &state = FindOrCreate(endpoint);
        VerifyOrReturnValue(index < chip::DeviceLayer::kMaxUserLabelListLength, CHIP_ERROR_INVALID_ARGUMENT);

        state.labels[index].label[0] = '\0';
        state.labels[index].value[0] = '\0';
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetUserLabelLength(chip::EndpointId endpoint, size_t val) override
    {
        VerifyOrReturnValue(val <= chip::DeviceLayer::kMaxUserLabelListLength, CHIP_ERROR_INVALID_ARGUMENT);
        EndpointUserLabelState &state = FindOrCreate(endpoint);
        state.length = val;
        for (size_t index = val; index < state.labels.size(); ++index) {
            state.labels[index].label[0] = '\0';
            state.labels[index].value[0] = '\0';
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetUserLabelLength(chip::EndpointId endpoint, size_t &val) override
    {
        const EndpointUserLabelState *state = FindOrNull(endpoint);
        val = state ? state->length : 0;
        return CHIP_NO_ERROR;
    }

private:
    static void CopySpan(chip::CharSpan source, char *destination, size_t destination_size)
    {
        const size_t count = std::min(source.size(), destination_size - 1);
        std::memcpy(destination, source.data(), count);
        destination[count] = '\0';
    }

    EndpointUserLabelState &FindOrCreate(chip::EndpointId endpoint)
    {
        for (EndpointUserLabelState &state : m_user_labels) {
            if (state.endpoint_id == endpoint || state.endpoint_id == chip::kInvalidEndpointId) {
                if (state.endpoint_id == chip::kInvalidEndpointId) {
                    state.endpoint_id = endpoint;
                }
                return state;
            }
        }

        return m_user_labels[0];
    }

    const EndpointUserLabelState *FindOrNull(chip::EndpointId endpoint) const
    {
        for (const EndpointUserLabelState &state : m_user_labels) {
            if (state.endpoint_id == endpoint) {
                return &state;
            }
        }
        return nullptr;
    }

    const FixedLabelEntry *m_entries = nullptr;
    size_t m_entry_count = 0;
    std::array<EndpointUserLabelState, kManagedEndpointCount> m_user_labels = {};
};

static matter_callbacks_t g_callbacks = {};
static matter_runtime_t *g_runtime = nullptr;
static bool g_internal_attribute_update = false;
static std::array<std::array<char, INPUT_NAME_MAX_LEN>, INPUT_SLOT_COUNT> g_default_input_labels = {};
static DeviceInfoProviderImpl g_device_info_provider;
static std::array<FixedLabelEntry, kManagedEndpointCount> g_fixed_labels = {};
static std::array<std::array<char, INPUT_NAME_MAX_LEN>, kManagedEndpointCount> g_fixed_label_values = {};
static node_t *g_node = nullptr;
static std::array<endpoint::on_off_light::config_t, INPUT_SLOT_COUNT> g_input_endpoint_configs = {};
static std::array<endpoint_t *, INPUT_SLOT_COUNT> g_input_endpoints = {};

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

static esp_err_t add_label_clusters(endpoint_t *endpoint)
{
    cluster::fixed_label::config_t fixed_label_cfg;
    cluster::user_label::config_t user_label_cfg;

    VerifyOrReturnValue(cluster::fixed_label::create(endpoint, &fixed_label_cfg, CLUSTER_FLAG_SERVER) != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to create Fixed Label cluster"));
    VerifyOrReturnValue(cluster::user_label::create(endpoint, &user_label_cfg, CLUSTER_FLAG_SERVER) != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to create User Label cluster"));
    return ESP_OK;
}

static esp_err_t resume_input_endpoint(size_t index, uint16_t endpoint_id)
{
    VerifyOrReturnValue(g_node != nullptr, ESP_ERR_INVALID_STATE, ESP_LOGE(TAG, "matter node unavailable"));

    endpoint_t *endpoint = endpoint::resume(g_node, ENDPOINT_FLAG_DESTROYABLE, endpoint_id, nullptr);
    VerifyOrReturnValue(endpoint != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to resume input endpoint %u", static_cast<unsigned int>(index)));
    VerifyOrReturnValue(cluster::descriptor::create(endpoint, &(g_input_endpoint_configs[index].descriptor),
                                                    CLUSTER_FLAG_SERVER) != nullptr,
                        ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to restore descriptor cluster for input endpoint %u",
                                 static_cast<unsigned int>(index)));
    ESP_RETURN_ON_ERROR(endpoint::on_off_light::add(endpoint, &(g_input_endpoint_configs[index])), TAG,
                        "input endpoint cluster add failed");
    ESP_RETURN_ON_ERROR(add_label_clusters(endpoint), TAG, "input label clusters failed");
    ESP_RETURN_ON_ERROR(endpoint::enable(endpoint), TAG, "input endpoint enable failed");

    g_input_endpoints[index] = endpoint;
    return ESP_OK;
}

static esp_err_t destroy_input_endpoint(size_t index)
{
    if (g_input_endpoints[index] == nullptr) {
        return ESP_OK;
    }

    VerifyOrReturnValue(g_node != nullptr, ESP_ERR_INVALID_STATE, ESP_LOGE(TAG, "matter node unavailable"));
    ESP_RETURN_ON_ERROR(endpoint::destroy(g_node, g_input_endpoints[index]), TAG, "input endpoint destroy failed");
    g_input_endpoints[index] = nullptr;
    return ESP_OK;
}

static void set_fixed_label(size_t index, uint16_t endpoint_id, const char *value)
{
    std::snprintf(g_fixed_label_values[index].data(), g_fixed_label_values[index].size(), "%s", value);
    g_fixed_labels[index].endpoint_id = endpoint_id;
    g_fixed_labels[index].label = chip::CharSpan::fromCharString(kLabelKey);
    g_fixed_labels[index].value = chip::CharSpan::fromCharString(g_fixed_label_values[index].data());
}

static esp_err_t seed_user_label(uint16_t endpoint_id, const char *value)
{
    chip::DeviceLayer::DeviceInfoProvider *provider = chip::DeviceLayer::GetDeviceInfoProvider();
    VerifyOrReturnValue(provider != nullptr, ESP_FAIL, ESP_LOGE(TAG, "DeviceInfoProvider is not available"));

    chip::DeviceLayer::DeviceInfoProvider::UserLabelType user_label = {};
    user_label.label = chip::CharSpan::fromCharString(kLabelKey);
    user_label.value = chip::CharSpan::fromCharString(value);

    std::array<chip::DeviceLayer::DeviceInfoProvider::UserLabelType, 1> user_labels = { user_label };

    CHIP_ERROR err = provider->SetUserLabelList(endpoint_id, chip::Span<const chip::DeviceLayer::DeviceInfoProvider::UserLabelType>(
                                                                 user_labels.data(), user_labels.size()));
    VerifyOrReturnValue(err == CHIP_NO_ERROR, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to seed User Label for endpoint %u: %" CHIP_ERROR_FORMAT, endpoint_id,
                                 err.Format()));
    return ESP_OK;
}

static esp_err_t sync_root_basic_information_metadata(const char *name)
{
    esp_matter_attr_val_t product_name_val = esp_matter_char_str(const_cast<char *>(name), strlen(name));
    esp_err_t err = attribute::update(kRootEndpointId, BasicInformation::Id,
                                      BasicInformation::Attributes::ProductName::Id, &product_name_val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "root product name update skipped: %s", esp_err_to_name(err));
    }

    esp_matter_attr_val_t product_label_val = esp_matter_char_str(const_cast<char *>(name), strlen(name));
    err = attribute::update(kRootEndpointId, BasicInformation::Id,
                            BasicInformation::Attributes::ProductLabel::Id, &product_label_val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "root product label update skipped: %s", esp_err_to_name(err));
    }
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
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb, nullptr);
    if (node == nullptr) {
        return ESP_FAIL;
    }

    g_node = node;
    g_callbacks = *callbacks;
    g_runtime = runtime;
    std::strncpy(g_runtime->device_name, config->monitor_name, sizeof(g_runtime->device_name) - 1);

    endpoint::dimmable_light::config_t brightness_cfg;
    brightness_cfg.on_off.on_off = true;
    brightness_cfg.level_control.current_level = 128;
    endpoint_t *brightness = endpoint::dimmable_light::create(node, &brightness_cfg, ENDPOINT_FLAG_NONE, nullptr);
    VerifyOrReturnValue(brightness != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create brightness endpoint"));

    endpoint::dimmable_light::config_t contrast_cfg;
    contrast_cfg.on_off.on_off = true;
    contrast_cfg.level_control.current_level = 128;
    endpoint_t *contrast = endpoint::dimmable_light::create(node, &contrast_cfg, ENDPOINT_FLAG_NONE, nullptr);
    VerifyOrReturnValue(contrast != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create contrast endpoint"));

    runtime->brightness_endpoint_id = endpoint::get_id(brightness);
    runtime->contrast_endpoint_id = endpoint::get_id(contrast);

    ESP_RETURN_ON_ERROR(add_label_clusters(brightness), TAG, "brightness label clusters failed");
    ESP_RETURN_ON_ERROR(add_label_clusters(contrast), TAG, "contrast label clusters failed");

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        g_input_endpoint_configs[index] = {};
        g_input_endpoint_configs[index].on_off.on_off = false;
        endpoint_t *input = endpoint::on_off_light::create(node, &g_input_endpoint_configs[index], ENDPOINT_FLAG_DESTROYABLE,
                                                           nullptr);
        VerifyOrReturnValue(input != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create input endpoint %u",
                                                                 static_cast<unsigned int>(index)));
        ESP_RETURN_ON_ERROR(add_label_clusters(input), TAG, "input label clusters failed");
        g_input_endpoints[index] = input;
        runtime->input_endpoint_ids[index] = endpoint::get_id(input);
    }

    set_fixed_label(0, runtime->brightness_endpoint_id, kBrightnessName);
    set_fixed_label(1, runtime->contrast_endpoint_id, kContrastName);
    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        set_fixed_label(2 + index, runtime->input_endpoint_ids[index], input_label_for_slot(config, index));
    }
    g_device_info_provider.SetFixedLabelEntries(g_fixed_labels.data(), g_fixed_labels.size());
    esp_matter::set_custom_device_info_provider(&g_device_info_provider);

    ESP_RETURN_ON_ERROR(esp_matter::start(app_event_cb), TAG, "esp_matter start failed");
    ESP_RETURN_ON_ERROR(sync_root_basic_information_metadata(kAccessoryName), TAG,
                        "root basic information metadata sync failed");
    ESP_RETURN_ON_ERROR(seed_user_label(runtime->brightness_endpoint_id, kBrightnessName), TAG,
                        "brightness user label seed failed");
    ESP_RETURN_ON_ERROR(seed_user_label(runtime->contrast_endpoint_id, kContrastName), TAG,
                        "contrast user label seed failed");
    ESP_RETURN_ON_ERROR(matter_sync_input_endpoints(config), TAG, "initial input endpoint sync failed");

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

    for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
        uint16_t endpoint_id = g_runtime->input_endpoint_ids[index];
        const char *label = input_label_for_slot(config, index);

        set_fixed_label(2 + index, endpoint_id, label);
        ESP_RETURN_ON_ERROR(seed_user_label(endpoint_id, label), TAG, "input user label seed failed");

        if (config->inputs[index].enabled) {
            if (g_input_endpoints[index] == nullptr) {
                ESP_RETURN_ON_ERROR(resume_input_endpoint(index, endpoint_id), TAG, "input endpoint resume failed");
            }
            ESP_RETURN_ON_ERROR(matter_update_input_state(endpoint_id, false), TAG, "input state reset failed");
        } else {
            if (g_input_endpoints[index] != nullptr) {
                ESP_RETURN_ON_ERROR(matter_update_input_state(endpoint_id, false), TAG, "input state reset failed");
                ESP_RETURN_ON_ERROR(destroy_input_endpoint(index), TAG, "input endpoint disable failed");
            }
        }
    }

    return ESP_OK;
}

extern "C" esp_err_t matter_open_basic_commissioning_window(void)
{
    CommissioningWindowRequest request = {
        .waiting_task = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(open_commissioning_window_work,
                                                                   reinterpret_cast<intptr_t>(&request));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "failed to schedule commissioning window work");
        return ESP_FAIL;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
        ESP_LOGW(TAG, "timed out waiting for commissioning window work");
        return ESP_ERR_TIMEOUT;
    }

    return request.result;
}

extern "C" bool matter_is_commissioned(void)
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}
