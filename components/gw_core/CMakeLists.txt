idf_component_register(
    SRCS
        "src/event_bus.c"
        "src/device_registry.c"
        "src/rules_engine.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_event
        log
)

