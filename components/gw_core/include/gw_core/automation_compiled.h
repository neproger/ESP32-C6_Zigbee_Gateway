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
This file defines the in-memory representation of a compiled automation,
and the functions to compile/serialize/deserialize them.

The fundamental binary structs (trigger, condition, action) are defined in types.h.
*/

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
