#include "gw_core/zb_classify.h"

#include <stdio.h>
#include <string.h>

// Common ZCL cluster IDs we care about for classification.
#define ZCL_CLUSTER_BASIC            0x0000
#define ZCL_CLUSTER_POWER_CONFIG     0x0001
#define ZCL_CLUSTER_GROUPS           0x0004
#define ZCL_CLUSTER_SCENES           0x0005
#define ZCL_CLUSTER_ONOFF            0x0006
#define ZCL_CLUSTER_LEVEL            0x0008
#define ZCL_CLUSTER_COLOR_CONTROL    0x0300
#define ZCL_CLUSTER_ILLUMINANCE      0x0400
#define ZCL_CLUSTER_TEMPERATURE      0x0402
#define ZCL_CLUSTER_PRESSURE         0x0403
#define ZCL_CLUSTER_FLOW             0x0404
#define ZCL_CLUSTER_HUMIDITY         0x0405
#define ZCL_CLUSTER_OCCUPANCY        0x0406

static bool cluster_list_has(const uint16_t *clusters, uint8_t count, uint16_t cluster_id)
{
    if (!clusters) {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static void json_write_str_array(const char *const *items, size_t count, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!items || count == 0) {
        (void)snprintf(out, out_size, "[]");
        return;
    }

    // Keep output valid JSON even when out_size is small: only append full items that fit.
    size_t used = 0;
    out[0] = '[';
    out[1] = '\0';
    used = 1;

    for (size_t i = 0; i < count; i++) {
        const char *s = items[i] ? items[i] : "";
        const size_t sl = strlen(s);
        const size_t needed = (i == 0 ? 0 : 1) + 2 + sl; // optional ',' + quotes + payload

        // Need room for item + closing ']' + '\0'
        if (used + needed + 2 > out_size) {
            break;
        }

        if (i != 0) {
            out[used++] = ',';
        }
        out[used++] = '"';
        memcpy(out + used, s, sl);
        used += sl;
        out[used++] = '"';
        out[used] = '\0';
    }

    if (used + 2 <= out_size) {
        out[used++] = ']';
        out[used] = '\0';
    } else {
        // Should not happen due to checks above, but keep it safe.
        out[out_size - 2] = ']';
        out[out_size - 1] = '\0';
    }
}

const char *gw_zb_endpoint_kind(const gw_zb_endpoint_t *ep)
{
    if (!ep) {
        return "unknown";
    }

    const bool onoff_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_ONOFF);
    const bool onoff_cli = cluster_list_has(ep->out_clusters, ep->out_cluster_count, ZCL_CLUSTER_ONOFF);
    const bool level_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_LEVEL);
    const bool color_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_COLOR_CONTROL);

    const bool temp_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_TEMPERATURE);
    const bool hum_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_HUMIDITY);
    const bool occ_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_OCCUPANCY);
    const bool illum_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_ILLUMINANCE);
    const bool press_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_PRESSURE);
    const bool flow_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_FLOW);

    // Actuators / lights
    if (color_srv) {
        return "color_light";
    }
    if (level_srv && onoff_srv) {
        return "dimmable_light";
    }
    if (onoff_srv) {
        return "relay";
    }

    // Controllers (emit commands)
    if (onoff_cli) {
        if (cluster_list_has(ep->out_clusters, ep->out_cluster_count, ZCL_CLUSTER_LEVEL)) {
            return "dimmer_switch";
        }
        return "switch";
    }

    // Sensors
    const bool any_sensor = temp_srv || hum_srv || occ_srv || illum_srv || press_srv || flow_srv;
    if (any_sensor) {
        if (temp_srv && hum_srv) return "temp_humidity_sensor";
        if (temp_srv) return "temperature_sensor";
        if (hum_srv) return "humidity_sensor";
        if (occ_srv) return "occupancy_sensor";
        if (illum_srv) return "illuminance_sensor";
        if (press_srv) return "pressure_sensor";
        if (flow_srv) return "flow_sensor";
        return "sensor";
    }

    return "unknown";
}

void gw_zb_endpoint_accepts_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size)
{
    const char *items[24];
    size_t n = 0;

    if (ep) {
        const bool onoff_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_ONOFF);
        const bool level_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_LEVEL);
        const bool color_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_COLOR_CONTROL);
        const bool groups_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_GROUPS);
        const bool scenes_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_SCENES);

        if (onoff_srv) {
            items[n++] = "onoff.off";
            items[n++] = "onoff.on";
            items[n++] = "onoff.toggle";
            items[n++] = "onoff.off_with_effect";
            items[n++] = "onoff.on_with_recall_global_scene";
            items[n++] = "onoff.on_with_timed_off";
        }
        if (level_srv) {
            items[n++] = "level.move_to_level";
            items[n++] = "level.move";
            items[n++] = "level.step";
            items[n++] = "level.stop";
            items[n++] = "level.move_to_level_with_onoff";
            items[n++] = "level.move_with_onoff";
            items[n++] = "level.step_with_onoff";
            items[n++] = "level.stop_with_onoff";
        }
        if (color_srv) {
            items[n++] = "color.move_to_hue";
            items[n++] = "color.move_hue";
            items[n++] = "color.step_hue";
            items[n++] = "color.move_to_saturation";
            items[n++] = "color.move_saturation";
            items[n++] = "color.step_saturation";
            items[n++] = "color.move_to_hue_saturation";
            items[n++] = "color.move_to_color_xy";
            items[n++] = "color.move_to_color_temperature";
            items[n++] = "color.stop_move_step";
        }
        if (groups_srv) {
            items[n++] = "groups.add";
            items[n++] = "groups.remove";
        }
        if (scenes_srv) {
            items[n++] = "scenes.recall";
        }
    }

    json_write_str_array(items, n, out, out_size);
}

void gw_zb_endpoint_emits_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size)
{
    const char *items[24];
    size_t n = 0;

    if (ep) {
        const bool onoff_cli = cluster_list_has(ep->out_clusters, ep->out_cluster_count, ZCL_CLUSTER_ONOFF);
        const bool level_cli = cluster_list_has(ep->out_clusters, ep->out_cluster_count, ZCL_CLUSTER_LEVEL);
        const bool color_cli = cluster_list_has(ep->out_clusters, ep->out_cluster_count, ZCL_CLUSTER_COLOR_CONTROL);

        if (onoff_cli) {
            items[n++] = "onoff.off";
            items[n++] = "onoff.on";
            items[n++] = "onoff.toggle";
        }
        if (level_cli) {
            items[n++] = "level.move_to_level";
            items[n++] = "level.move";
            items[n++] = "level.step";
            items[n++] = "level.stop";
            items[n++] = "level.move_to_level_with_onoff";
            items[n++] = "level.move_with_onoff";
            items[n++] = "level.step_with_onoff";
            items[n++] = "level.stop_with_onoff";
        }
        if (color_cli) {
            items[n++] = "color.*";
        }
    }

    json_write_str_array(items, n, out, out_size);
}

void gw_zb_endpoint_reports_json(const gw_zb_endpoint_t *ep, char *out, size_t out_size)
{
    const char *items[16];
    size_t n = 0;

    if (ep) {
        const bool onoff_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_ONOFF);
        const bool level_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_LEVEL);
        const bool temp_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_TEMPERATURE);
        const bool hum_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_HUMIDITY);
        const bool occ_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_OCCUPANCY);
        const bool illum_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_ILLUMINANCE);
        const bool power_srv = cluster_list_has(ep->in_clusters, ep->in_cluster_count, ZCL_CLUSTER_POWER_CONFIG);

        if (onoff_srv) items[n++] = "onoff";
        if (level_srv) items[n++] = "level";
        if (temp_srv) items[n++] = "temperature_c";
        if (hum_srv) items[n++] = "humidity_pct";
        if (occ_srv) items[n++] = "occupancy";
        if (illum_srv) items[n++] = "illuminance";
        if (power_srv) items[n++] = "battery_pct";
    }

    json_write_str_array(items, n, out, out_size);
}
