idf_component_register(SRCS "esp_zigbee_gateway.c"
                            "gw_wifi.c"
                    PRIV_REQUIRES ieee802154 esp_driver_usb_serial_jtag esp_coex esp_netif
                                  vfs esp_wifi nvs_flash esp_driver_uart esp_timer
                                  gw_zigbee
                                  gw_core
                                  gw_http
                    INCLUDE_DIRS ".")
