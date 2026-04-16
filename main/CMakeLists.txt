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
    EMBED_FILES
        "../frontend/index.html"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
