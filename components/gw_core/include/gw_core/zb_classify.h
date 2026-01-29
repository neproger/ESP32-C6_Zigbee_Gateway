#pragma once

#include <stddef.h>

#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// A human-friendly classification for a single endpoint, derived from its Simple Descriptor.
//
// Note: "device type" is profile-specific; this is a practical heuristic based on ZCL clusters
// present on the endpoint (server clusters => accepts commands/reports; client clusters => emits commands).
const char *gw_zb_endpoint_kind(const gw_zb_endpoint_t *ep);

// Returns JSON array strings like ["onoff.on","onoff.off"] into out (always null-terminated).
// If there are no actions, returns "[]".
void gw_zb_endpoint_accepts_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size);
void gw_zb_endpoint_emits_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size);
void gw_zb_endpoint_reports_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

