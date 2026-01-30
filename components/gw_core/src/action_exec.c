#include "gw_core/action_exec.h"

#include <stdlib.h>
#include <string.h>

#include "gw_zigbee/gw_zigbee.h"

#include "gw_core/types.h"

static void set_err(char *err, size_t err_size, const char *msg)
{
    if (!err || err_size == 0) {
        return;
    }
    if (!msg) {
        err[0] = '\0';
        return;
    }
    strncpy(err, msg, err_size);
    err[err_size - 1] = '\0';
}

static bool parse_u16(cJSON *j, uint16_t *out)
{
    if (!out) return false;
    if (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 65535) {
        *out = (uint16_t)j->valuedouble;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0] != '\0') {
        char *end = NULL;
        unsigned long v = strtoul(j->valuestring, &end, 0);
        if (end && *end == '\0' && v <= 65535UL) {
            *out = (uint16_t)v;
            return true;
        }
    }
    return false;
}

static bool parse_u8(cJSON *j, uint8_t *out, uint8_t min_v, uint8_t max_v)
{
    if (!out) return false;
    if (!cJSON_IsNumber(j)) return false;
    if (j->valuedouble < (double)min_v || j->valuedouble > (double)max_v) return false;
    *out = (uint8_t)j->valuedouble;
    return true;
}

static bool parse_u16_ms(cJSON *j, uint16_t *out, uint16_t max_v)
{
    if (!out) return false;
    if (!cJSON_IsNumber(j)) return false;
    if (j->valuedouble < 0 || j->valuedouble > (double)max_v) return false;
    *out = (uint16_t)j->valuedouble;
    return true;
}

static bool parse_uid(cJSON *j, gw_device_uid_t *out)
{
    if (!out) return false;
    if (!cJSON_IsString(j) || !j->valuestring || j->valuestring[0] == '\0') return false;
    memset(out, 0, sizeof(*out));
    strlcpy(out->uid, j->valuestring, sizeof(out->uid));
    return true;
}

static esp_err_t exec_onoff_unicast(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *uid_j = cJSON_GetObjectItemCaseSensitive(action, "device_uid");
    cJSON *ep_j = cJSON_GetObjectItemCaseSensitive(action, "endpoint");
    if (!uid_j) uid_j = cJSON_GetObjectItemCaseSensitive(action, "uid");

    gw_device_uid_t uid = {0};
    if (!parse_uid(uid_j, &uid)) {
        set_err(err, err_size, "missing device_uid");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t endpoint = 0;
    if (!parse_u8(ep_j, &endpoint, 1, 240)) {
        set_err(err, err_size, "bad endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
    if (strcmp(cmd, "onoff.off") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
    else if (strcmp(cmd, "onoff.on") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
    else if (strcmp(cmd, "onoff.toggle") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
    else {
        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }

    return gw_zigbee_onoff_cmd(&uid, endpoint, ocmd);
}

static esp_err_t exec_level_unicast(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    if (strcmp(cmd, "level.move_to_level") != 0) {
        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *uid_j = cJSON_GetObjectItemCaseSensitive(action, "device_uid");
    cJSON *ep_j = cJSON_GetObjectItemCaseSensitive(action, "endpoint");
    cJSON *level_j = cJSON_GetObjectItemCaseSensitive(action, "level");
    cJSON *transition_ms_j = cJSON_GetObjectItemCaseSensitive(action, "transition_ms");

    gw_device_uid_t uid = {0};
    if (!parse_uid(uid_j, &uid)) {
        set_err(err, err_size, "missing device_uid");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t endpoint = 0;
    if (!parse_u8(ep_j, &endpoint, 1, 240)) {
        set_err(err, err_size, "bad endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t level = 0;
    if (!parse_u8(level_j, &level, 0, 254)) {
        set_err(err, err_size, "bad level");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t transition_ms = 0;
    if (transition_ms_j && !cJSON_IsNull(transition_ms_j)) {
        if (!parse_u16_ms(transition_ms_j, &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }
    }

    gw_zigbee_level_t p = {.level = level, .transition_ms = transition_ms};
    return gw_zigbee_level_move_to_level(&uid, endpoint, p);
}

static esp_err_t exec_color_unicast(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *uid_j = cJSON_GetObjectItemCaseSensitive(action, "device_uid");
    cJSON *ep_j = cJSON_GetObjectItemCaseSensitive(action, "endpoint");
    cJSON *transition_ms_j = cJSON_GetObjectItemCaseSensitive(action, "transition_ms");

    gw_device_uid_t uid = {0};
    if (!parse_uid(uid_j, &uid)) {
        set_err(err, err_size, "missing device_uid");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t endpoint = 0;
    if (!parse_u8(ep_j, &endpoint, 1, 240)) {
        set_err(err, err_size, "bad endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t transition_ms = 0;
    if (transition_ms_j && !cJSON_IsNull(transition_ms_j)) {
        if (!parse_u16_ms(transition_ms_j, &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (strcmp(cmd, "color.move_to_color_xy") == 0) {
        cJSON *x_j = cJSON_GetObjectItemCaseSensitive(action, "x");
        cJSON *y_j = cJSON_GetObjectItemCaseSensitive(action, "y");
        uint16_t x = 0, y = 0;
        if (!parse_u16(x_j, &x)) {
            set_err(err, err_size, "bad x");
            return ESP_ERR_INVALID_ARG;
        }
        if (!parse_u16(y_j, &y)) {
            set_err(err, err_size, "bad y");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_color_xy_t p = {.x = x, .y = y, .transition_ms = transition_ms};
        return gw_zigbee_color_move_to_xy(&uid, endpoint, p);
    }

    if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
        cJSON *mireds_j = cJSON_GetObjectItemCaseSensitive(action, "mireds");
        uint16_t mireds = 0;
        if (!parse_u16(mireds_j, &mireds) || mireds < 1 || mireds > 1000) {
            set_err(err, err_size, "bad mireds");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_color_temp_t p = {.mireds = mireds, .transition_ms = transition_ms};
        return gw_zigbee_color_move_to_temp(&uid, endpoint, p);
    }

    set_err(err, err_size, "bad cmd");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t exec_group_onoff(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *gid_j = cJSON_GetObjectItemCaseSensitive(action, "group_id");
    uint16_t gid = 0;
    if (!parse_u16(gid_j, &gid) || gid == 0 || gid == 0xFFFF) {
        set_err(err, err_size, "bad group_id");
        return ESP_ERR_INVALID_ARG;
    }

    gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
    if (strcmp(cmd, "onoff.off") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
    else if (strcmp(cmd, "onoff.on") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
    else if (strcmp(cmd, "onoff.toggle") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
    else {
        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }

    return gw_zigbee_group_onoff_cmd(gid, ocmd);
}

static esp_err_t exec_group_level(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    if (strcmp(cmd, "level.move_to_level") != 0) {
        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *gid_j = cJSON_GetObjectItemCaseSensitive(action, "group_id");
    cJSON *level_j = cJSON_GetObjectItemCaseSensitive(action, "level");
    cJSON *transition_ms_j = cJSON_GetObjectItemCaseSensitive(action, "transition_ms");

    uint16_t gid = 0;
    if (!parse_u16(gid_j, &gid) || gid == 0 || gid == 0xFFFF) {
        set_err(err, err_size, "bad group_id");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t level = 0;
    if (!parse_u8(level_j, &level, 0, 254)) {
        set_err(err, err_size, "bad level");
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t transition_ms = 0;
    if (transition_ms_j && !cJSON_IsNull(transition_ms_j)) {
        if (!parse_u16_ms(transition_ms_j, &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }
    }

    gw_zigbee_level_t p = {.level = level, .transition_ms = transition_ms};
    return gw_zigbee_group_level_move_to_level(gid, p);
}

static esp_err_t exec_group_color(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *gid_j = cJSON_GetObjectItemCaseSensitive(action, "group_id");
    cJSON *transition_ms_j = cJSON_GetObjectItemCaseSensitive(action, "transition_ms");
    uint16_t gid = 0;
    if (!parse_u16(gid_j, &gid) || gid == 0 || gid == 0xFFFF) {
        set_err(err, err_size, "bad group_id");
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t transition_ms = 0;
    if (transition_ms_j && !cJSON_IsNull(transition_ms_j)) {
        if (!parse_u16_ms(transition_ms_j, &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (strcmp(cmd, "color.move_to_color_xy") == 0) {
        cJSON *x_j = cJSON_GetObjectItemCaseSensitive(action, "x");
        cJSON *y_j = cJSON_GetObjectItemCaseSensitive(action, "y");
        uint16_t x = 0, y = 0;
        if (!parse_u16(x_j, &x)) {
            set_err(err, err_size, "bad x");
            return ESP_ERR_INVALID_ARG;
        }
        if (!parse_u16(y_j, &y)) {
            set_err(err, err_size, "bad y");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_color_xy_t p = {.x = x, .y = y, .transition_ms = transition_ms};
        return gw_zigbee_group_color_move_to_xy(gid, p);
    }

    if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
        cJSON *mireds_j = cJSON_GetObjectItemCaseSensitive(action, "mireds");
        uint16_t mireds = 0;
        if (!parse_u16(mireds_j, &mireds) || mireds < 1 || mireds > 1000) {
            set_err(err, err_size, "bad mireds");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_color_temp_t p = {.mireds = mireds, .transition_ms = transition_ms};
        return gw_zigbee_group_color_move_to_temp(gid, p);
    }

    set_err(err, err_size, "bad cmd");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t exec_scene(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *gid_j = cJSON_GetObjectItemCaseSensitive(action, "group_id");
    cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(action, "scene_id");
    uint16_t gid = 0;
    if (!parse_u16(gid_j, &gid) || gid == 0 || gid == 0xFFFF) {
        set_err(err, err_size, "bad group_id");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t sid = 0;
    if (!parse_u8(sid_j, &sid, 1, 255)) {
        set_err(err, err_size, "bad scene_id");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(cmd, "scene.store") == 0) {
        return gw_zigbee_scene_store(gid, sid);
    }
    if (strcmp(cmd, "scene.recall") == 0) {
        return gw_zigbee_scene_recall(gid, sid);
    }

    set_err(err, err_size, "bad cmd");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t exec_binding(const char *cmd, cJSON *action, char *err, size_t err_size)
{
    cJSON *src_uid_j = cJSON_GetObjectItemCaseSensitive(action, "src_device_uid");
    cJSON *src_ep_j = cJSON_GetObjectItemCaseSensitive(action, "src_endpoint");
    cJSON *cluster_j = cJSON_GetObjectItemCaseSensitive(action, "cluster_id");
    cJSON *dst_uid_j = cJSON_GetObjectItemCaseSensitive(action, "dst_device_uid");
    cJSON *dst_ep_j = cJSON_GetObjectItemCaseSensitive(action, "dst_endpoint");

    // allow shorter names
    if (!src_uid_j) src_uid_j = cJSON_GetObjectItemCaseSensitive(action, "src_uid");
    if (!dst_uid_j) dst_uid_j = cJSON_GetObjectItemCaseSensitive(action, "dst_uid");

    gw_device_uid_t src_uid = {0};
    gw_device_uid_t dst_uid = {0};
    if (!parse_uid(src_uid_j, &src_uid)) {
        set_err(err, err_size, "missing src_device_uid");
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_uid(dst_uid_j, &dst_uid)) {
        set_err(err, err_size, "missing dst_device_uid");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t src_ep = 0, dst_ep = 0;
    if (!parse_u8(src_ep_j, &src_ep, 1, 240)) {
        set_err(err, err_size, "bad src_endpoint");
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_u8(dst_ep_j, &dst_ep, 1, 240)) {
        set_err(err, err_size, "bad dst_endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t cluster_id = 0;
    if (!parse_u16(cluster_j, &cluster_id) || cluster_id == 0) {
        set_err(err, err_size, "bad cluster_id");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(cmd, "bind") == 0) {
        return gw_zigbee_bind(&src_uid, src_ep, cluster_id, &dst_uid, dst_ep);
    }
    if (strcmp(cmd, "unbind") == 0) {
        return gw_zigbee_unbind(&src_uid, src_ep, cluster_id, &dst_uid, dst_ep);
    }

    set_err(err, err_size, "bad cmd");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t gw_action_exec(cJSON *action, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!cJSON_IsObject(action)) {
        set_err(err, err_size, "action must be object");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(action, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring) {
        set_err(err, err_size, "missing type");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(type_j->valuestring, "zigbee") != 0) {
        set_err(err, err_size, "unsupported type");
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(action, "cmd");
    if (!cJSON_IsString(cmd_j) || !cmd_j->valuestring || cmd_j->valuestring[0] == '\0') {
        set_err(err, err_size, "missing cmd");
        return ESP_ERR_INVALID_ARG;
    }
    const char *cmd = cmd_j->valuestring;

    // Decision: group vs device is based on presence of group_id vs device_uid.
    const bool has_group = cJSON_GetObjectItemCaseSensitive(action, "group_id") != NULL;
    const bool has_uid = cJSON_GetObjectItemCaseSensitive(action, "device_uid") != NULL || cJSON_GetObjectItemCaseSensitive(action, "uid") != NULL;

    if (strcmp(cmd, "scene.store") == 0 || strcmp(cmd, "scene.recall") == 0) {
        return exec_scene(cmd, action, err, err_size);
    }
    if (strcmp(cmd, "bind") == 0 || strcmp(cmd, "unbind") == 0) {
        return exec_binding(cmd, action, err, err_size);
    }

    if (strncmp(cmd, "onoff.", 6) == 0) {
        return has_group ? exec_group_onoff(cmd, action, err, err_size) : exec_onoff_unicast(cmd, action, err, err_size);
    }
    if (strncmp(cmd, "level.", 6) == 0) {
        return has_group ? exec_group_level(cmd, action, err, err_size) : exec_level_unicast(cmd, action, err, err_size);
    }
    if (strncmp(cmd, "color.", 6) == 0) {
        return has_group ? exec_group_color(cmd, action, err, err_size) : exec_color_unicast(cmd, action, err, err_size);
    }

    // Back-compat: old format {cmd:"on"/"off"/"toggle", cluster:"0x0006", device_uid...}
    if (has_uid && (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0 || strcmp(cmd, "toggle") == 0)) {
        char tmp[32];
        (void)snprintf(tmp, sizeof(tmp), "onoff.%s", cmd);
        return exec_onoff_unicast(tmp, action, err, err_size);
    }
    if (has_group && (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0 || strcmp(cmd, "toggle") == 0)) {
        char tmp[32];
        (void)snprintf(tmp, sizeof(tmp), "onoff.%s", cmd);
        return exec_group_onoff(tmp, action, err, err_size);
    }

    set_err(err, err_size, "unknown cmd");
    return ESP_ERR_NOT_SUPPORTED;
}

