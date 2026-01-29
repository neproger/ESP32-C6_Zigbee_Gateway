idf_component_register(
    SRCS
        "src/gw_http.c"
        "src/gw_ws.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_http_server
        json
        log
        spiffs
        gw_core
        gw_zigbee
)

