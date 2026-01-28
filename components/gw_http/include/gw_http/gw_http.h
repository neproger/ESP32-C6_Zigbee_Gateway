#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_http_start(void);
esp_err_t gw_http_stop(void);
uint16_t gw_http_get_port(void);

#ifdef __cplusplus
}
#endif

