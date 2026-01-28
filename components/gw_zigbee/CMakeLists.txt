idf_component_register(
    SRCS
        "src/gw_zigbee.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        log
        esp_timer
        gw_core
        espressif__esp-zigbee-lib
)

