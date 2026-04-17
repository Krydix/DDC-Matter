#include "matter.h"

#include <array>
#include <cstdio>
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <platform/PlatformManager.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/DeviceInfoProvider.h>
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
static constexpr char kAccessoryName[] = "Display Control";
static constexpr char kLabelKey[] = "name";
static constexpr char kBrightnessName[] = "Brightness";
static constexpr char kContrastName[] = "Contrast";
static constexpr char kInputName[] = "Input Source";
static constexpr uint32_t kCommissioningWindowTimeoutSecs = 15 * 60;

static uint8_t ddc_level_to_matter_level(uint8_t ddc_level)
{
    uint8_t clamped = std::min<uint8_t>(ddc_level, 100);
    return static_cast<uint8_t>((static_cast<uint16_t>(clamped) * 254U + 50U) / 100U);
}

struct CommissioningWindowRequest {
    TaskHandle_t waiting_task;
    esp_err_t result;
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

class InputModesManager : public chip::app::Clusters::ModeSelect::SupportedModesManager {
public:
    using ModeOptionStructType = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
    using SemanticTagType = chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type;

    void Update(chip::EndpointId endpoint_id, const display_config_t *config)
    {
        m_endpoint_id = endpoint_id;
        m_count = INPUT_SLOT_COUNT;
        for (size_t index = 0; index < INPUT_SLOT_COUNT; ++index) {
            const char *name = config->inputs[index].name[0] != '\0' ? config->inputs[index].name : nullptr;
            if (name == nullptr) {
                std::snprintf(m_labels[index].data(), m_labels[index].size(), "Input %u", (unsigned int)(index + 1));
            } else {
                std::snprintf(m_labels[index].data(), m_labels[index].size(), "%s", name);
            }

            m_modes[index].label = chip::CharSpan::fromCharString(m_labels[index].data());
            m_modes[index].mode = static_cast<uint8_t>(index);
            m_modes[index].semanticTags = chip::app::DataModel::List<const SemanticTagType>(m_empty_semantic_tags.data(), 0);
        }
    }

    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpoint_id) const override
    {
        if (endpoint_id != m_endpoint_id || m_count == 0) {
            return ModeOptionsProvider();
        }
        return ModeOptionsProvider(m_modes.data(), m_modes.data() + m_count);
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(chip::EndpointId endpoint_id, uint8_t mode,
                                                                  const ModeOptionStructType **data_ptr) const override
    {
        if (endpoint_id != m_endpoint_id) {
            return chip::Protocols::InteractionModel::Status::UnsupportedCluster;
        }

        for (size_t index = 0; index < m_count; ++index) {
            if (m_modes[index].mode == mode) {
                *data_ptr = &m_modes[index];
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }

        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }

private:
    chip::EndpointId m_endpoint_id = chip::kInvalidEndpointId;
    size_t m_count = 0;
    std::array<SemanticTagType, 1> m_empty_semantic_tags = {};
    std::array<std::array<char, INPUT_NAME_MAX_LEN>, INPUT_SLOT_COUNT> m_labels = {};
    std::array<ModeOptionStructType, INPUT_SLOT_COUNT> m_modes = {};
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
        UserLabelIteratorImpl(const EndpointUserLabelState *state) : m_state(state) {}

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
    std::array<EndpointUserLabelState, 3> m_user_labels = {};
};

static matter_callbacks_t g_callbacks = {};
static matter_runtime_t *g_runtime = nullptr;
static bool g_internal_attribute_update = false;
static DeviceInfoProviderImpl g_device_info_provider;
static std::array<FixedLabelEntry, 3> g_fixed_labels;
static InputModesManager g_input_modes_manager;

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

static void set_fixed_label(size_t index, uint16_t endpoint_id, const char *value)
{
    g_fixed_labels[index].endpoint_id = endpoint_id;
    g_fixed_labels[index].label = chip::CharSpan::fromCharString(kLabelKey);
    g_fixed_labels[index].value = chip::CharSpan::fromCharString(value);
}

static esp_err_t seed_user_label(uint16_t endpoint_id, const char *value)
{
    chip::DeviceLayer::DeviceInfoProvider *provider = chip::DeviceLayer::GetDeviceInfoProvider();
    VerifyOrReturnValue(provider != nullptr, ESP_FAIL, ESP_LOGE(TAG, "DeviceInfoProvider is not available"));

    chip::DeviceLayer::DeviceInfoProvider::UserLabelType user_label = {};
    user_label.label = chip::CharSpan::fromCharString(kLabelKey);
    user_label.value = chip::CharSpan::fromCharString(value);

    CHIP_ERROR err = provider->SetUserLabelList(endpoint_id, chip::Span<const chip::DeviceLayer::DeviceInfoProvider::UserLabelType>(&user_label, 1));
    VerifyOrReturnValue(err == CHIP_NO_ERROR, ESP_FAIL,
                        ESP_LOGE(TAG, "Failed to seed User Label for endpoint %u: %" CHIP_ERROR_FORMAT, endpoint_id,
                                 err.Format()));
    return ESP_OK;
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
    if (cluster_id == ModeSelect::Id && attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        return g_callbacks.mode_write ? g_callbacks.mode_write(val->val.u8, g_callbacks.ctx) : ESP_OK;
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

    endpoint_t *input = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
    VerifyOrReturnValue(input != nullptr, ESP_FAIL, ESP_LOGE(TAG, "Failed to create input endpoint"));
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

    ESP_RETURN_ON_ERROR(add_label_clusters(brightness), TAG, "brightness label clusters failed");
    ESP_RETURN_ON_ERROR(add_label_clusters(contrast), TAG, "contrast label clusters failed");
    ESP_RETURN_ON_ERROR(add_label_clusters(input), TAG, "input label clusters failed");

    set_fixed_label(0, runtime->brightness_endpoint_id, kBrightnessName);
    set_fixed_label(1, runtime->contrast_endpoint_id, kContrastName);
    set_fixed_label(2, runtime->input_endpoint_id, kInputName);
    g_device_info_provider.SetFixedLabelEntries(g_fixed_labels.data(), g_fixed_labels.size());
    esp_matter::set_custom_device_info_provider(&g_device_info_provider);
    chip::app::Clusters::ModeSelect::setSupportedModesManager(&g_input_modes_manager);
    g_input_modes_manager.Update(runtime->input_endpoint_id, config);

    ESP_RETURN_ON_ERROR(esp_matter::start(app_event_cb), TAG, "esp_matter start failed");
    ESP_RETURN_ON_ERROR(seed_user_label(runtime->brightness_endpoint_id, kBrightnessName), TAG,
                        "brightness user label seed failed");
    ESP_RETURN_ON_ERROR(seed_user_label(runtime->contrast_endpoint_id, kContrastName), TAG,
                        "contrast user label seed failed");
    ESP_RETURN_ON_ERROR(seed_user_label(runtime->input_endpoint_id, kInputName), TAG,
                        "input user label seed failed");

    return ESP_OK;
}

extern "C" esp_err_t matter_update_level(uint16_t endpoint_id, uint8_t level)
{
    InternalAttributeUpdateGuard guard;
    esp_matter_attr_val_t val = esp_matter_uint8(ddc_level_to_matter_level(level));
    return attribute::update(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);
}

extern "C" esp_err_t matter_update_mode(uint16_t endpoint_id, uint8_t mode)
{
    InternalAttributeUpdateGuard guard;
    esp_matter_attr_val_t val = esp_matter_uint8(mode);
    return attribute::update(endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &val);
}

extern "C" esp_err_t matter_update_supported_modes(const display_config_t *config, uint16_t endpoint_id)
{
    g_input_modes_manager.Update(endpoint_id, config);
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
