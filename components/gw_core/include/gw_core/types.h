#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// IEEE (EUI‑64) as string: "0x00124B0012345678" + '\0'
#define GW_DEVICE_UID_STRLEN 19

typedef struct {
    char uid[GW_DEVICE_UID_STRLEN];
} gw_device_uid_t;

typedef struct {
    gw_device_uid_t device_uid; // stable (IEEE)
    uint16_t short_addr;        // current network address (may change after rejoin)
    uint8_t endpoint;
} gw_device_ref_t;

#ifdef __cplusplus
}
#endif

