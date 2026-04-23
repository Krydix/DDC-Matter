set(app_srcs
    "ddc.c"
    "edid.c"
    "mccs.c")

set(app_requires
    driver
    freertos)

set(app_embed_files)

if(DDC_STANDALONE_DEBUG)
    list(APPEND app_srcs "debug_serial_app.cpp")
else()
    list(APPEND app_srcs
        "app_main.cpp"
        "config.c"
        "matter.cpp"
        "monitor_db.c"
        "webserver.c")
    list(APPEND app_requires
        esp_matter
        mdns
        nvs_flash
        esp_http_client
        esp_http_server
        json
        esp-tls)
    list(APPEND app_embed_files "../frontend/index.html")
endif()

idf_component_register(
    SRCS
        ${app_srcs}
    INCLUDE_DIRS
        "."
    REQUIRES
        ${app_requires}
    EMBED_FILES
        ${app_embed_files}
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
