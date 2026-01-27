idf_component_register(
    SRCS
        "src/gw_http.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_http_server
        log
        gw_core
)

