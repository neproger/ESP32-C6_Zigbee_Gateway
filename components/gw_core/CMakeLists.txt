idf_component_register(
    SRCS
        "src/event_bus.c"
        "src/device_registry.c"
        "src/zb_model.c"
        "src/sensor_store.c"
        "src/rules_engine.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_event
        esp_timer
        log
)

