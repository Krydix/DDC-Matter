cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif()

if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32_devkit_c)
endif()

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

project(ddc_matter_display_controller)

# Older esp-matter / CHIP ESP32 headers use #if on Bluetooth Kconfig symbols.
# Add fallback values only for the CHIP component so -Wundef does not break
# those third-party sources while leaving ESP-IDF's own #ifdef checks intact.
idf_component_get_property(chip_component_lib chip COMPONENT_LIB)

if(chip_component_lib)
    if(NOT CONFIG_BT_NIMBLE_ENABLED)
        target_compile_definitions(${chip_component_lib} PRIVATE CONFIG_BT_NIMBLE_ENABLED=0)
    endif()

    if(NOT CONFIG_BT_BLUEDROID_ENABLED)
        target_compile_definitions(${chip_component_lib} PRIVATE CONFIG_BT_BLUEDROID_ENABLED=0)
    endif()
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os;-Wno-error=unused-result" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
