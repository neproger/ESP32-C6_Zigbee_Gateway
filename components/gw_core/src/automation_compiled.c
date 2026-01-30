#include "gw_core/automation_compiled.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define MAGIC_GWAR 0x52415747u // 'GWAR'

static void set_err(char *out, size_t out_size, const char *msg)
{
    if (!out || out_size == 0) return;
    if (!msg) {
        out[0] = '\0';
        return;
    }
    strncpy(out, msg, out_size);
    out[out_size - 1] = '\0';
}

static uint16_t parse_u16_any(const cJSON *j, bool *ok)
{
    if (ok) *ok = false;
    if (cJSON_IsNumber(j)) {
        double v = j->valuedouble;
        if (v >= 0 && v <= 65535) {
            if (ok) *ok = true;
            return (uint16_t)v;
        }
        return 0;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        char *end = NULL;
        unsigned long v = strtoul(j->valuestring, &end, 0);
        if (end && *end == '\0' && v <= 65535UL) {
            if (ok) *ok = true;
            return (uint16_t)v;
        }
    }
    return 0;
}

static uint32_t parse_u32_any(const cJSON *j, bool *ok)
{
    if (ok) *ok = false;
    if (cJSON_IsNumber(j)) {
        double v = j->valuedouble;
        if (v >= 0 && v <= 4294967295.0) {
            if (ok) *ok = true;
            return (uint32_t)v;
        }
        return 0;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        char *end = NULL;
        unsigned long v = strtoul(j->valuestring, &end, 0);
        if (end && *end == '\0') {
            if (ok) *ok = true;
            return (uint32_t)v;
        }
    }
    return 0;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strtab_t;

static esp_err_t strtab_init(strtab_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    t->buf = (char *)calloc(1, 1);
    if (!t->buf) return ESP_ERR_NO_MEM;
    t->len = 1; // offset 0 => ""
    t->cap = 1;
    return ESP_OK;
}

static void strtab_free(strtab_t *t)
{
    if (!t) return;
    free(t->buf);
    *t = (strtab_t){0};
}

static uint32_t strtab_add(strtab_t *t, const char *s)
{
    if (!t || !t->buf || !s || !s[0]) return 0;
    // naive de-dupe: linear scan (fine for MVP; can hash later)
    for (size_t off = 0; off < t->len;) {
        const char *cur = t->buf + off;
        size_t n = strlen(cur);
        if (strcmp(cur, s) == 0) {
            return (uint32_t)off;
        }
        off += n + 1;
    }

    size_t n = strlen(s) + 1;
    if (t->len + n > t->cap) {
        size_t next = t->cap ? t->cap : 1;
        while (next < t->len + n) next *= 2;
        char *nb = (char *)realloc(t->buf, next);
        if (!nb) return 0;
        t->buf = nb;
        t->cap = next;
    }
    uint32_t off = (uint32_t)t->len;
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    return off;
}

static gw_auto_evt_type_t evt_type_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "zigbee.command") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(s, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(s, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(s, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static gw_auto_op_t op_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "==") == 0) return GW_AUTO_OP_EQ;
    if (strcmp(s, "!=") == 0) return GW_AUTO_OP_NE;
    if (strcmp(s, ">") == 0) return GW_AUTO_OP_GT;
    if (strcmp(s, "<") == 0) return GW_AUTO_OP_LT;
    if (strcmp(s, ">=") == 0) return GW_AUTO_OP_GE;
    if (strcmp(s, "<=") == 0) return GW_AUTO_OP_LE;
    return 0;
}

static esp_err_t compile_one(const char *json, gw_auto_compiled_t *out, char *err, size_t err_size)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_err(err, err_size, "bad json");
        return ESP_ERR_INVALID_ARG;
    }

    strtab_t st = {0};
    esp_err_t rc = strtab_init(&st);
    if (rc != ESP_OK) {
        cJSON_Delete(root);
        set_err(err, err_size, "no mem");
        return rc;
    }

    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *triggers_j = cJSON_GetObjectItemCaseSensitive(root, "triggers");
    const cJSON *conds_j = cJSON_GetObjectItemCaseSensitive(root, "conditions");
    const cJSON *actions_j = cJSON_GetObjectItemCaseSensitive(root, "actions");

    if (!cJSON_IsString(id_j) || !id_j->valuestring || !id_j->valuestring[0]) {
        set_err(err, err_size, "missing id");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cJSON_IsString(name_j) || !name_j->valuestring) {
        set_err(err, err_size, "missing name");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cJSON_IsArray(triggers_j)) {
        set_err(err, err_size, "missing triggers");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cJSON_IsArray(conds_j)) {
        // allow empty
    }
    if (!cJSON_IsArray(actions_j)) {
        set_err(err, err_size, "missing actions");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }

    // Counts
    const uint32_t trigger_count = (uint32_t)cJSON_GetArraySize((cJSON *)triggers_j);
    const uint32_t cond_count = cJSON_IsArray(conds_j) ? (uint32_t)cJSON_GetArraySize((cJSON *)conds_j) : 0;
    const uint32_t action_count = (uint32_t)cJSON_GetArraySize((cJSON *)actions_j);

    gw_auto_bin_automation_v2_t *auto_rec = (gw_auto_bin_automation_v2_t *)calloc(1, sizeof(*auto_rec));
    gw_auto_bin_trigger_v2_t *trigs = trigger_count ? (gw_auto_bin_trigger_v2_t *)calloc(trigger_count, sizeof(*trigs)) : NULL;
    gw_auto_bin_condition_v2_t *conds = cond_count ? (gw_auto_bin_condition_v2_t *)calloc(cond_count, sizeof(*conds)) : NULL;
    gw_auto_bin_action_v2_t *acts = action_count ? (gw_auto_bin_action_v2_t *)calloc(action_count, sizeof(*acts)) : NULL;
    if (!auto_rec || (trigger_count && !trigs) || (cond_count && !conds) || (action_count && !acts)) {
        set_err(err, err_size, "no mem");
        rc = ESP_ERR_NO_MEM;
        free(auto_rec);
        free(trigs);
        free(conds);
        free(acts);
        goto done;
    }

    auto_rec->id_off = strtab_add(&st, id_j->valuestring);
    auto_rec->name_off = strtab_add(&st, name_j->valuestring);
    auto_rec->enabled = cJSON_IsBool(enabled_j) ? (cJSON_IsTrue(enabled_j) ? 1 : 0) : 1;
    auto_rec->mode = (cJSON_IsString(mode_j) && mode_j->valuestring && strcmp(mode_j->valuestring, "single") == 0) ? 1 : 1;
    auto_rec->triggers_index = 0;
    auto_rec->triggers_count = trigger_count;
    auto_rec->conditions_index = 0;
    auto_rec->conditions_count = cond_count;
    auto_rec->actions_index = 0;
    auto_rec->actions_count = action_count;

    // Triggers
    for (uint32_t i = 0; i < trigger_count; i++) {
        const cJSON *t = cJSON_GetArrayItem((cJSON *)triggers_j, (int)i);
        if (!cJSON_IsObject(t)) {
            set_err(err, err_size, "trigger must be object");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        const cJSON *type_j2 = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "type");
        const cJSON *event_type_j = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "event_type");
        const cJSON *match_j = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "match");
        if (!cJSON_IsString(type_j2) || !type_j2->valuestring || strcmp(type_j2->valuestring, "event") != 0) {
            set_err(err, err_size, "unsupported trigger.type");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        if (!cJSON_IsString(event_type_j) || !event_type_j->valuestring) {
            set_err(err, err_size, "missing trigger.event_type");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        gw_auto_evt_type_t et = evt_type_from_str(event_type_j->valuestring);
        if (!et) {
            set_err(err, err_size, "unsupported event_type");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        trigs[i].event_type = (uint8_t)et;
        trigs[i].endpoint = 0;
        trigs[i].device_uid_off = 0;
        trigs[i].cmd_off = 0;
        trigs[i].cluster_id = 0;
        trigs[i].attr_id = 0;

        if (cJSON_IsObject(match_j)) {
            const cJSON *uid_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "device_uid");
            if (cJSON_IsString(uid_m) && uid_m->valuestring && uid_m->valuestring[0]) {
                trigs[i].device_uid_off = strtab_add(&st, uid_m->valuestring);
            }

            const cJSON *ep_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "payload.endpoint");
            if (ep_m) {
                bool ok16 = false;
                uint16_t v = parse_u16_any(ep_m, &ok16);
                if (ok16 && v <= 255) {
                    trigs[i].endpoint = (uint8_t)v;
                }
            }

            if (et == GW_AUTO_EVT_ZIGBEE_COMMAND) {
                const cJSON *cmd_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "payload.cmd");
                if (cJSON_IsString(cmd_m) && cmd_m->valuestring && cmd_m->valuestring[0]) {
                    trigs[i].cmd_off = strtab_add(&st, cmd_m->valuestring);
                }
                const cJSON *cluster_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "payload.cluster");
                bool ok16 = false;
                uint16_t cid = parse_u16_any(cluster_m, &ok16);
                if (ok16) trigs[i].cluster_id = cid;
            } else if (et == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
                const cJSON *cluster_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "payload.cluster");
                const cJSON *attr_m = cJSON_GetObjectItemCaseSensitive((cJSON *)match_j, "payload.attr");
                bool okc = false;
                bool oka = false;
                uint16_t cid = parse_u16_any(cluster_m, &okc);
                uint16_t aid = parse_u16_any(attr_m, &oka);
                if (okc) trigs[i].cluster_id = cid;
                if (oka) trigs[i].attr_id = aid;
            }
        }
    }

    // Conditions
    for (uint32_t i = 0; i < cond_count; i++) {
        const cJSON *c = cJSON_GetArrayItem((cJSON *)conds_j, (int)i);
        if (!cJSON_IsObject(c)) {
            set_err(err, err_size, "condition must be object");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        const cJSON *type_j2 = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "type");
        const cJSON *op_j = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "op");
        const cJSON *ref_j = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "ref");
        const cJSON *value_j = cJSON_GetObjectItemCaseSensitive((cJSON *)c, "value");
        if (!cJSON_IsString(type_j2) || !type_j2->valuestring || strcmp(type_j2->valuestring, "state") != 0) {
            set_err(err, err_size, "unsupported condition.type");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        if (!cJSON_IsString(op_j) || !op_j->valuestring) {
            set_err(err, err_size, "missing condition.op");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        if (!cJSON_IsObject(ref_j)) {
            set_err(err, err_size, "missing condition.ref");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        const cJSON *uid_j = cJSON_GetObjectItemCaseSensitive((cJSON *)ref_j, "device_uid");
        const cJSON *key_j = cJSON_GetObjectItemCaseSensitive((cJSON *)ref_j, "key");
        if (!cJSON_IsString(uid_j) || !uid_j->valuestring || !uid_j->valuestring[0]) {
            set_err(err, err_size, "missing condition.ref.device_uid");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        if (!cJSON_IsString(key_j) || !key_j->valuestring || !key_j->valuestring[0]) {
            set_err(err, err_size, "missing condition.ref.key");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        gw_auto_op_t op = op_from_str(op_j->valuestring);
        if (!op) {
            set_err(err, err_size, "bad condition.op");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        conds[i].op = (uint8_t)op;
        conds[i].device_uid_off = strtab_add(&st, uid_j->valuestring);
        conds[i].key_off = strtab_add(&st, key_j->valuestring);

        if (cJSON_IsBool(value_j)) {
            conds[i].val_type = GW_AUTO_VAL_BOOL;
            conds[i].v.b = cJSON_IsTrue(value_j) ? 1 : 0;
        } else if (cJSON_IsNumber(value_j)) {
            conds[i].val_type = GW_AUTO_VAL_F64;
            conds[i].v.f64 = value_j->valuedouble;
        } else if (cJSON_IsString(value_j) && value_j->valuestring && value_j->valuestring[0]) {
            // Try parse as double
            char *end = NULL;
            double v = strtod(value_j->valuestring, &end);
            if (end && *end == '\0') {
                conds[i].val_type = GW_AUTO_VAL_F64;
                conds[i].v.f64 = v;
            } else {
                set_err(err, err_size, "bad condition.value");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
        } else {
            set_err(err, err_size, "bad condition.value");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
    }

    // Actions (Zigbee primitives, compiled)
    for (uint32_t i = 0; i < action_count; i++) {
        const cJSON *a = cJSON_GetArrayItem((cJSON *)actions_j, (int)i);
        if (!cJSON_IsObject(a)) {
            set_err(err, err_size, "action must be object");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        const cJSON *type_j2 = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "type");
        const cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "cmd");
        if (!cJSON_IsString(type_j2) || !type_j2->valuestring || strcmp(type_j2->valuestring, "zigbee") != 0) {
            set_err(err, err_size, "unsupported action.type");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        if (!cJSON_IsString(cmd_j) || !cmd_j->valuestring || !cmd_j->valuestring[0]) {
            set_err(err, err_size, "missing action.cmd");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        const char *cmd = cmd_j->valuestring;
        acts[i].cmd_off = strtab_add(&st, cmd);

        // 1) Binding / unbinding (ZDO)
        if (strcmp(cmd, "bind") == 0 || strcmp(cmd, "unbind") == 0 ||
            strcmp(cmd, "bindings.bind") == 0 || strcmp(cmd, "bindings.unbind") == 0) {
            const cJSON *src_uid_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "src_device_uid");
            const cJSON *src_ep_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "src_endpoint");
            const cJSON *cluster_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "cluster_id");
            const cJSON *dst_uid_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "dst_device_uid");
            const cJSON *dst_ep_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "dst_endpoint");

            if (!cJSON_IsString(src_uid_j) || !src_uid_j->valuestring || !src_uid_j->valuestring[0]) {
                set_err(err, err_size, "missing action.src_device_uid");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
            if (!cJSON_IsString(dst_uid_j) || !dst_uid_j->valuestring || !dst_uid_j->valuestring[0]) {
                set_err(err, err_size, "missing action.dst_device_uid");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            bool ok_src_ep = false;
            uint16_t src_ep = parse_u16_any(src_ep_j, &ok_src_ep);
            if (!ok_src_ep || src_ep == 0 || src_ep > 255) {
                set_err(err, err_size, "bad action.src_endpoint");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            bool ok_dst_ep = false;
            uint16_t dst_ep = parse_u16_any(dst_ep_j, &ok_dst_ep);
            if (!ok_dst_ep || dst_ep == 0 || dst_ep > 255) {
                set_err(err, err_size, "bad action.dst_endpoint");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            bool ok_cluster = false;
            uint16_t cluster_id = parse_u16_any(cluster_j, &ok_cluster);
            if (!ok_cluster || cluster_id == 0) {
                set_err(err, err_size, "bad action.cluster_id");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            acts[i].kind = GW_AUTO_ACT_BIND;
            acts[i].uid_off = strtab_add(&st, src_uid_j->valuestring);
            acts[i].uid2_off = strtab_add(&st, dst_uid_j->valuestring);
            acts[i].endpoint = (uint8_t)src_ep;
            acts[i].aux_ep = (uint8_t)dst_ep;
            acts[i].u16_0 = cluster_id;
            acts[i].flags = (strstr(cmd, "unbind") != NULL) ? GW_AUTO_ACT_FLAG_UNBIND : 0;
            continue;
        }

        // 2) Scenes (group-based)
        if (strcmp(cmd, "scene.store") == 0 || strcmp(cmd, "scene.recall") == 0) {
            const cJSON *group_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "group_id");
            const cJSON *scene_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "scene_id");

            bool ok_gid = false;
            uint16_t group_id = parse_u16_any(group_j, &ok_gid);
            if (!ok_gid || group_id == 0 || group_id == 0xFFFF) {
                set_err(err, err_size, "bad action.group_id");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            bool ok_scene = false;
            uint32_t scene_id = parse_u32_any(scene_j, &ok_scene);
            if (!ok_scene || scene_id == 0 || scene_id > 255) {
                set_err(err, err_size, "bad action.scene_id");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }

            acts[i].kind = GW_AUTO_ACT_SCENE;
            acts[i].u16_0 = group_id;
            acts[i].u16_1 = (uint16_t)scene_id;
            continue;
        }

        // 3) Group actions (groupcast) -- detected by presence of group_id
        const cJSON *group_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "group_id");
        bool ok_gid = false;
        uint16_t group_id = parse_u16_any(group_j, &ok_gid);
        if (ok_gid && group_id != 0 && group_id != 0xFFFF) {
            acts[i].kind = GW_AUTO_ACT_GROUP;
            acts[i].u16_0 = group_id;

            if (strcmp(cmd, "level.move_to_level") == 0) {
                const cJSON *lvl_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "level");
                const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");
                bool ok_lvl = false;
                uint32_t lvl = parse_u32_any(lvl_j, &ok_lvl);
                if (!ok_lvl || lvl > 254) {
                    set_err(err, err_size, "bad action.level");
                    rc = ESP_ERR_INVALID_ARG;
                    goto done_alloc;
                }
                bool ok_tr = false;
                uint32_t tr = parse_u32_any(tr_j, &ok_tr);
                acts[i].arg0_u32 = lvl;
                acts[i].arg1_u32 = ok_tr ? tr : 0;
            } else if (strcmp(cmd, "color.move_to_color_xy") == 0) {
                const cJSON *x_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "x");
                const cJSON *y_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "y");
                const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");

                bool ok_x = false;
                bool ok_y = false;
                uint32_t x = parse_u32_any(x_j, &ok_x);
                uint32_t y = parse_u32_any(y_j, &ok_y);
                if (!ok_x || x > 65535) {
                    set_err(err, err_size, "bad action.x");
                    rc = ESP_ERR_INVALID_ARG;
                    goto done_alloc;
                }
                if (!ok_y || y > 65535) {
                    set_err(err, err_size, "bad action.y");
                    rc = ESP_ERR_INVALID_ARG;
                    goto done_alloc;
                }
                bool ok_tr = false;
                uint32_t tr = parse_u32_any(tr_j, &ok_tr);
                acts[i].arg0_u32 = x;
                acts[i].arg1_u32 = y;
                acts[i].arg2_u32 = ok_tr ? tr : 0;
            } else if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
                const cJSON *m_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "mireds");
                const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");

                bool ok_m = false;
                uint32_t mireds = parse_u32_any(m_j, &ok_m);
                if (!ok_m || mireds < 1 || mireds > 1000) {
                    set_err(err, err_size, "bad action.mireds");
                    rc = ESP_ERR_INVALID_ARG;
                    goto done_alloc;
                }
                bool ok_tr = false;
                uint32_t tr = parse_u32_any(tr_j, &ok_tr);
                acts[i].arg0_u32 = mireds;
                acts[i].arg1_u32 = ok_tr ? tr : 0;
            }
            continue;
        }

        // 4) Device actions (unicast)
        const cJSON *uid_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "device_uid");
        const cJSON *ep_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "endpoint");
        if (!cJSON_IsString(uid_j) || !uid_j->valuestring || !uid_j->valuestring[0]) {
            set_err(err, err_size, "missing action.device_uid");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }
        bool ok_ep = false;
        uint16_t ep = parse_u16_any(ep_j, &ok_ep);
        if (!ok_ep || ep == 0 || ep > 255) {
            set_err(err, err_size, "bad action.endpoint");
            rc = ESP_ERR_INVALID_ARG;
            goto done_alloc;
        }

        acts[i].kind = GW_AUTO_ACT_DEVICE;
        acts[i].uid_off = strtab_add(&st, uid_j->valuestring);
        acts[i].endpoint = (uint8_t)ep;

        if (strcmp(cmd, "level.move_to_level") == 0) {
            const cJSON *lvl_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "level");
            const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");
            bool ok_lvl = false;
            uint32_t lvl = parse_u32_any(lvl_j, &ok_lvl);
            if (!ok_lvl || lvl > 254) {
                set_err(err, err_size, "bad action.level");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
            bool ok_tr = false;
            uint32_t tr = parse_u32_any(tr_j, &ok_tr);
            acts[i].arg0_u32 = lvl;
            acts[i].arg1_u32 = ok_tr ? tr : 0;
        } else if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            const cJSON *x_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "x");
            const cJSON *y_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "y");
            const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");

            bool ok_x = false;
            bool ok_y = false;
            uint32_t x = parse_u32_any(x_j, &ok_x);
            uint32_t y = parse_u32_any(y_j, &ok_y);
            if (!ok_x || x > 65535) {
                set_err(err, err_size, "bad action.x");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
            if (!ok_y || y > 65535) {
                set_err(err, err_size, "bad action.y");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
            bool ok_tr = false;
            uint32_t tr = parse_u32_any(tr_j, &ok_tr);
            acts[i].arg0_u32 = x;
            acts[i].arg1_u32 = y;
            acts[i].arg2_u32 = ok_tr ? tr : 0;
        } else if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            const cJSON *m_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "mireds");
            const cJSON *tr_j = cJSON_GetObjectItemCaseSensitive((cJSON *)a, "transition_ms");

            bool ok_m = false;
            uint32_t mireds = parse_u32_any(m_j, &ok_m);
            if (!ok_m || mireds < 1 || mireds > 1000) {
                set_err(err, err_size, "bad action.mireds");
                rc = ESP_ERR_INVALID_ARG;
                goto done_alloc;
            }
            bool ok_tr = false;
            uint32_t tr = parse_u32_any(tr_j, &ok_tr);
            acts[i].arg0_u32 = mireds;
            acts[i].arg1_u32 = ok_tr ? tr : 0;
        }
    }

    // Populate output (single automation bundle)
    memset(out, 0, sizeof(*out));
    out->hdr.magic = MAGIC_GWAR;
    out->hdr.version = 2;
    out->hdr.automation_count = 1;
    out->hdr.trigger_count_total = trigger_count;
    out->hdr.condition_count_total = cond_count;
    out->hdr.action_count_total = action_count;

    out->autos = auto_rec;
    out->triggers = trigs;
    out->conditions = conds;
    out->actions = acts;
    out->strings = st.buf;
    st.buf = NULL;
    out->hdr.strings_size = (uint32_t)st.len;

    rc = ESP_OK;
    goto done;

done_alloc:
    free(auto_rec);
    free(trigs);
    free(conds);
    free(acts);
done:
    strtab_free(&st);
    cJSON_Delete(root);
    return rc;
}

esp_err_t gw_auto_compile_json(const char *json, gw_auto_compiled_t *out, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!json || !out) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }
    return compile_one(json, out, err, err_size);
}

void gw_auto_compiled_free(gw_auto_compiled_t *c)
{
    if (!c) return;
    free(c->autos);
    free(c->triggers);
    free(c->conditions);
    free(c->actions);
    free(c->strings);
    *c = (gw_auto_compiled_t){0};
}

esp_err_t gw_auto_compiled_serialize(const gw_auto_compiled_t *c, uint8_t **out_buf, size_t *out_len)
{
    if (!c || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    if (c->hdr.magic != MAGIC_GWAR || c->hdr.version != 2) return ESP_ERR_INVALID_ARG;

    const size_t hdr_sz = sizeof(gw_auto_bin_header_v2_t);
    const size_t autos_sz = (size_t)c->hdr.automation_count * sizeof(gw_auto_bin_automation_v2_t);
    const size_t tr_sz = (size_t)c->hdr.trigger_count_total * sizeof(gw_auto_bin_trigger_v2_t);
    const size_t co_sz = (size_t)c->hdr.condition_count_total * sizeof(gw_auto_bin_condition_v2_t);
    const size_t ac_sz = (size_t)c->hdr.action_count_total * sizeof(gw_auto_bin_action_v2_t);
    const size_t st_sz = (size_t)c->hdr.strings_size;

    gw_auto_bin_header_v2_t hdr = c->hdr;
    hdr.automations_off = (uint32_t)hdr_sz;
    hdr.triggers_off = (uint32_t)(hdr_sz + autos_sz);
    hdr.conditions_off = (uint32_t)(hdr.triggers_off + tr_sz);
    hdr.actions_off = (uint32_t)(hdr.conditions_off + co_sz);
    hdr.strings_off = (uint32_t)(hdr.actions_off + ac_sz);
    hdr.strings_size = (uint32_t)st_sz;

    const size_t total = hdr_sz + autos_sz + tr_sz + co_sz + ac_sz + st_sz;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return ESP_ERR_NO_MEM;

    // Header: memcpy is OK (same target arch), but keep magic/version explicit to avoid surprises.
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + hdr.automations_off, c->autos, autos_sz);
    memcpy(buf + hdr.triggers_off, c->triggers, tr_sz);
    memcpy(buf + hdr.conditions_off, c->conditions, co_sz);
    memcpy(buf + hdr.actions_off, c->actions, ac_sz);
    memcpy(buf + hdr.strings_off, c->strings, st_sz);

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

esp_err_t gw_auto_compiled_deserialize(const uint8_t *buf, size_t len, gw_auto_compiled_t *out)
{
    if (!buf || !out || len < sizeof(gw_auto_bin_header_v2_t)) return ESP_ERR_INVALID_ARG;

    gw_auto_bin_header_v2_t hdr = {0};
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != MAGIC_GWAR || hdr.version != 2) return ESP_ERR_INVALID_ARG;

    // Basic bounds checks
    if (hdr.strings_off > len || hdr.strings_size > len || hdr.strings_off + hdr.strings_size > len) return ESP_ERR_INVALID_ARG;

    const size_t autos_sz = (size_t)hdr.automation_count * sizeof(gw_auto_bin_automation_v2_t);
    const size_t tr_sz = (size_t)hdr.trigger_count_total * sizeof(gw_auto_bin_trigger_v2_t);
    const size_t co_sz = (size_t)hdr.condition_count_total * sizeof(gw_auto_bin_condition_v2_t);
    const size_t ac_sz = (size_t)hdr.action_count_total * sizeof(gw_auto_bin_action_v2_t);

    if ((size_t)hdr.automations_off + autos_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.triggers_off + tr_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.conditions_off + co_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.actions_off + ac_sz > len) return ESP_ERR_INVALID_ARG;

    gw_auto_compiled_t c = {0};
    c.hdr = hdr;
    c.autos = hdr.automation_count ? (gw_auto_bin_automation_v2_t *)calloc(hdr.automation_count, sizeof(*c.autos)) : NULL;
    c.triggers = hdr.trigger_count_total ? (gw_auto_bin_trigger_v2_t *)calloc(hdr.trigger_count_total, sizeof(*c.triggers)) : NULL;
    c.conditions = hdr.condition_count_total ? (gw_auto_bin_condition_v2_t *)calloc(hdr.condition_count_total, sizeof(*c.conditions)) : NULL;
    c.actions = hdr.action_count_total ? (gw_auto_bin_action_v2_t *)calloc(hdr.action_count_total, sizeof(*c.actions)) : NULL;
    c.strings = hdr.strings_size ? (char *)calloc(1, hdr.strings_size) : NULL;

    if ((hdr.automation_count && !c.autos) || (hdr.trigger_count_total && !c.triggers) || (hdr.condition_count_total && !c.conditions) ||
        (hdr.action_count_total && !c.actions) || (hdr.strings_size && !c.strings)) {
        gw_auto_compiled_free(&c);
        return ESP_ERR_NO_MEM;
    }

    memcpy(c.autos, buf + hdr.automations_off, autos_sz);
    memcpy(c.triggers, buf + hdr.triggers_off, tr_sz);
    memcpy(c.conditions, buf + hdr.conditions_off, co_sz);
    memcpy(c.actions, buf + hdr.actions_off, ac_sz);
    memcpy(c.strings, buf + hdr.strings_off, hdr.strings_size);

    *out = c;
    return ESP_OK;
}

esp_err_t gw_auto_compiled_write_file(const char *path, const gw_auto_compiled_t *c)
{
    if (!path || !c) return ESP_ERR_INVALID_ARG;
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t err = gw_auto_compiled_serialize(c, &buf, &len);
    if (err != ESP_OK) return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }
    size_t w = fwrite(buf, 1, len, f);
    (void)fclose(f);
    free(buf);
    return (w == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_auto_compiled_read_file(const char *path, gw_auto_compiled_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)sz);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    esp_err_t err = gw_auto_compiled_deserialize(buf, (size_t)sz, out);
    free(buf);
    return err;
}
