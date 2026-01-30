#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Binary format for compiled automations (V2)
=========================================

Goal:
- Keep runtime execution fast and low-allocation (no per-event parsing of automation JSON).
- Keep the format human-readable for developers (simple structs, explicit offsets, comments).
- Make it versioned and extensible.

We store a compiled file per automation (default: "/data/<automation_id>.gwar").
The on-disk format supports N automations in one file, but today we write/read it
as a single-automation bundle (automation_count=1). This keeps it easy to
inspect and update incrementally.

Each compiled file contains:
  1) header
  2) N automation records
  3) string table (zero-terminated strings)

All integers are little-endian. Offsets are file offsets in bytes.

File layout:

  +------------------------------+
  | gw_auto_bin_header_v2_t      |
  +------------------------------+
  | gw_auto_bin_automation_v2_t  |  count times
  +------------------------------+
  | gw_auto_bin_trigger_v2_t     |  trigger_count_total times
  +------------------------------+
  | gw_auto_bin_condition_v2_t   |  condition_count_total times
  +------------------------------+
  | gw_auto_bin_action_v2_t      |  action_count_total times
  +------------------------------+
  | string_table bytes           |
  +------------------------------+

Strings:
- Stored once in the string table as UTF-8 with trailing '\0'.
- Referenced by u32 offsets from the start of the string table.
- Offset 0 means "" (empty string).

Why "string table" instead of inlining strings?
- Keeps records fixed-size and easy to scan.
- Allows sharing (device_uid repeated across many rules).

Notes:
- This is an internal storage/execution format. UI continues using JSON.

Quick mental model (why it's not an “array of mixed values”)
------------------------------------------------------------
Instead of something like:
  ["dev_id", 0x0006, 0, 1]
we store typed records in typed arrays, plus a shared string table.

So at runtime we do:
- read one `gw_auto_bin_trigger_v2_t` record
- compare `event_type`, `endpoint`, `cluster_id`, ...
- look up strings by offset in the string table (e.g. `device_uid_off`, `cmd_off`)
This stays fast and avoids allocating/parsing automation JSON on every event.
*/

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

typedef struct {
    uint32_t magic;   // 'GWAR' = 0x52415747
    uint16_t version; // 2
    uint16_t reserved;

    uint32_t automation_count;
    uint32_t trigger_count_total;
    uint32_t condition_count_total;
    uint32_t action_count_total;

    uint32_t automations_off; // offset to automations array
    uint32_t triggers_off;    // offset to triggers array
    uint32_t conditions_off;  // offset to conditions array
    uint32_t actions_off;     // offset to actions array
    uint32_t strings_off;     // offset to string table
    uint32_t strings_size;    // size of string table in bytes
} gw_auto_bin_header_v2_t;

typedef struct {
    uint32_t id_off;   // string table offset
    uint32_t name_off; // string table offset
    uint8_t enabled;   // 0/1
    uint8_t mode;      // reserved for future (single/parallel/etc)
    uint16_t reserved;

    uint32_t triggers_index;    // base index into triggers array
    uint32_t triggers_count;
    uint32_t conditions_index;  // base index into conditions array
    uint32_t conditions_count;
    uint32_t actions_index;     // base index into actions array
    uint32_t actions_count;
} gw_auto_bin_automation_v2_t;

typedef struct {
    uint8_t event_type; // gw_auto_evt_type_t
    uint8_t endpoint;   // 0 = any
    uint16_t reserved;

    uint32_t device_uid_off; // string table offset (0 = any)

    // Match fields (optional, depends on event_type):
    //
    // For zigbee.command:
    //   cmd_off: payload.cmd string ("toggle"/"on"/"off"/"move_to_level"/...)
    //
    // For zigbee.attr_report:
    //   cluster_id + attr_id match payload.cluster/payload.attr
    //
    // For device.join/leave:
    //   currently only device_uid/endpoint are used (endpoint is typically 0)
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

    // Value payload: either f64 or bool
    union {
        double f64;
        uint8_t b;
    } v;
} gw_auto_bin_condition_v2_t;

typedef struct {
    // Compiled action record (Zigbee primitives only for now).
    //
    // This is a fixed-size record designed to stay stable and easy to extend.
    // We store enough fields to execute actions without parsing JSON at runtime.
    //
    // The meaning of fields depends on `kind`, but we keep a consistent layout:
    // - cmd_off always points to a command string ("onoff.toggle", "scene.recall", "bind", ...)
    // - uid_off / uid2_off are string table offsets for IEEE addresses
    // - endpoint / aux_ep are endpoint numbers
    // - u16_0/u16_1 + arg*_u32 are generic numeric slots for parameters

    // What kind of target/action this is.
    // - DEVICE: unicast to device endpoint (on/off/level/...)
    // - GROUP: groupcast to group_id (on/off/level/...)
    // - SCENE: group-based scene store/recall
    // - BIND: ZDO bind/unbind (src cluster -> dst endpoint)
    uint8_t kind;     // gw_auto_act_kind_t
    uint8_t endpoint; // device endpoint OR bind src_endpoint (0 if unused)
    uint8_t aux_ep;   // bind dst_endpoint (0 if unused)
    uint8_t flags;    // kind-specific flags (see below)

    // Small numeric parameters:
    // - GROUP/SCENE: u16_0=group_id
    // - BIND: u16_0=cluster_id
    // - SCENE: u16_1=scene_id (1..255)
    uint16_t u16_0;
    uint16_t u16_1;

    uint32_t cmd_off;  // string table offset (required)
    uint32_t uid_off;  // DEVICE: device_uid; BIND: src_device_uid; else 0
    uint32_t uid2_off; // BIND: dst_device_uid; else 0

    // Generic numeric args:
    // - level.move_to_level: arg0_u32=level (0..254), arg1_u32=transition_ms (0..60000)
    uint32_t arg0_u32;
    uint32_t arg1_u32;
    uint32_t arg2_u32;
} gw_auto_bin_action_v2_t;

typedef enum {
    GW_AUTO_ACT_DEVICE = 1,
    GW_AUTO_ACT_GROUP = 2,
    GW_AUTO_ACT_SCENE = 3,
    GW_AUTO_ACT_BIND = 4,
} gw_auto_act_kind_t;

// gw_auto_bin_action_v2_t.flags
typedef enum {
    GW_AUTO_ACT_FLAG_UNBIND = 1 << 0, // BIND: unbind instead of bind
} gw_auto_act_flag_t;

typedef struct {
    // In-memory representation produced by the compiler (points into owned buffers).
    gw_auto_bin_header_v2_t hdr;
    gw_auto_bin_automation_v2_t *autos;
    gw_auto_bin_trigger_v2_t *triggers;
    gw_auto_bin_condition_v2_t *conditions;
    gw_auto_bin_action_v2_t *actions;
    char *strings; // string table bytes
} gw_auto_compiled_t;

// Compile a JSON automation definition into compiled (binary-friendly) representation.
// - `json` is the full automation JSON (same shape as UI emits).
// - On success, `out` owns allocations and must be freed with gw_auto_compiled_free().
esp_err_t gw_auto_compile_json(const char *json, gw_auto_compiled_t *out, char *err, size_t err_size);

void gw_auto_compiled_free(gw_auto_compiled_t *c);

// Serialize compiled representation into a contiguous binary buffer (malloc'ed).
esp_err_t gw_auto_compiled_serialize(const gw_auto_compiled_t *c, uint8_t **out_buf, size_t *out_len);

// Deserialize a compiled buffer into heap-owned structures (use gw_auto_compiled_free()).
esp_err_t gw_auto_compiled_deserialize(const uint8_t *buf, size_t len, gw_auto_compiled_t *out);

// Convenience: read/write compiled automations file.
esp_err_t gw_auto_compiled_write_file(const char *path, const gw_auto_compiled_t *c);
esp_err_t gw_auto_compiled_read_file(const char *path, gw_auto_compiled_t *out);

#ifdef __cplusplus
}
#endif
