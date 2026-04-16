idf_component_register(
    SRCS
        "app_main.cpp"
        "config.c"
        "ddc.c"
        "edid.c"
        "matter.cpp"
        "mccs.c"
        "monitor_db.c"
        "webserver.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_matter
        mdns
        driver
        nvs_flash
        esp_http_client
        esp_http_server
        json
        esp-tls
        freertos
    EMBED_FILES
        "../frontend/index.html"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
