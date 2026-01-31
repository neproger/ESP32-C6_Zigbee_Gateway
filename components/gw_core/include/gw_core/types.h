#pragma once

#include <stdbool.h>
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

// New definitions for automations, migrated and redesigned for efficiency.
// ========================================================================

#define GW_AUTOMATION_ID_MAX   32
#define GW_AUTOMATION_NAME_MAX 48

// These limits are a trade-off between flexibility and memory usage.
// They can be tuned after analyzing typical use cases.
#define GW_AUTO_MAX_TRIGGERS           4
#define GW_AUTO_MAX_CONDITIONS         8
#define GW_AUTO_MAX_ACTIONS            8
#define GW_AUTO_MAX_STRING_TABLE_BYTES 256

// --- Low-level binary structs and enums for automations (moved from automation_compiled.h) ---

typedef enum {
    GW_AUTO_EVT_ZIGBEE_COMMAND = 1,
    GW_AUTO_EVT_ZIGBEE_ATTR_REPORT = 2,
    GW_AUTO_EVT_DEVICE_JOIN = 3,
    GW_AUTO_EVT_DEVICE_LEAVE = 4,
} gw_auto_evt_type_t;

typedef enum {
    GW_AUTO_OP_EQ = 1,
    GW_AUTO_OP_NE = 2,
    GW_AUTO_OP_GT = 3,
    GW_AUTO_OP_LT = 4,
    GW_AUTO_OP_GE = 5,
    GW_AUTO_OP_LE = 6,
} gw_auto_op_t;

typedef enum {
    GW_AUTO_VAL_F64 = 1,
    GW_AUTO_VAL_BOOL = 2,
} gw_auto_val_type_t;

typedef enum {
    GW_AUTO_ACT_DEVICE = 1,
    GW_AUTO_ACT_GROUP = 2,
    GW_AUTO_ACT_SCENE = 3,
    GW_AUTO_ACT_BIND = 4,
    GW_AUTO_ACT_MGMT = 5,
} gw_auto_act_kind_t;

typedef enum {
    GW_AUTO_ACT_FLAG_UNBIND = 1 << 0, // BIND: unbind instead of bind
    GW_AUTO_ACT_FLAG_REJOIN = 1 << 1, // MGMT leave: request rejoin
} gw_auto_act_flag_t;

typedef struct {
    uint8_t event_type; // gw_auto_evt_type_t
    uint8_t endpoint;   // 0 = any
    uint16_t reserved;
    uint32_t device_uid_off; // string table offset (0 = any)
    uint32_t cmd_off;    // string table offset (0 = any)
    uint16_t cluster_id; // 0 = any
    uint16_t attr_id;    // 0 = any
} gw_auto_bin_trigger_v2_t;

typedef struct {
    uint8_t op;        // gw_auto_op_t
    uint8_t val_type;  // gw_auto_val_type_t
    uint16_t reserved;
    uint32_t device_uid_off; // string table offset (required)
    uint32_t key_off;        // string table offset (required), e.g. "temperature_c"
    union {
        double f64;
        uint8_t b;
    } v;
} gw_auto_bin_condition_v2_t;

typedef struct {
    uint8_t kind;     // gw_auto_act_kind_t
    uint8_t endpoint; // device endpoint OR bind src_endpoint (0 if unused)
    uint8_t aux_ep;   // bind dst_endpoint (0 if unused)
    uint8_t flags;    // kind-specific flags (see below)
    uint16_t u16_0;
    uint16_t u16_1;
    uint32_t cmd_off;  // string table offset (required)
    uint32_t uid_off;  // DEVICE: device_uid; BIND: src_device_uid; else 0
    uint32_t uid2_off; // BIND: dst_device_uid; else 0
    uint32_t arg0_u32;
    uint32_t arg1_u32;
    uint32_t arg2_u32;
} gw_auto_bin_action_v2_t;

// --- End of moved structs ---

// New, self-contained structure representing a single compiled automation.
// It has a fixed size and contains no pointers, which simplifies serialization.
typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
    char name[GW_AUTOMATION_NAME_MAX];
    bool enabled;
    uint8_t reserved; // Padding/alignment

    uint8_t triggers_count;
    uint8_t conditions_count;
    uint8_t actions_count;
    uint8_t reserved2; // Padding/alignment

    gw_auto_bin_trigger_v2_t triggers[GW_AUTO_MAX_TRIGGERS];
    gw_auto_bin_condition_v2_t conditions[GW_AUTO_MAX_CONDITIONS];
    gw_auto_bin_action_v2_t actions[GW_AUTO_MAX_ACTIONS];

    uint16_t string_table_size;
    char string_table[GW_AUTO_MAX_STRING_TABLE_BYTES];
} gw_automation_entry_t;

// Lightweight metadata view for UI/status, does not need the full compiled body.
typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
    char name[GW_AUTOMATION_NAME_MAX];
    bool enabled;
} gw_automation_meta_t;


#ifdef __cplusplus
}
#endif

