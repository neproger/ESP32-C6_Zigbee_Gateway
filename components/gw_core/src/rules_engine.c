#include "gw_core/rules_engine.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/action_exec.h"
#include "gw_core/automation_compiled.h"
#include "gw_core/automation_store.h"
#include "gw_core/state_store.h"

static const char *TAG = "gw_rules";

static bool s_inited;
static QueueHandle_t s_q;
static TaskHandle_t s_task;

typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
    gw_auto_compiled_t compiled;
} gw_rule_entry_t;

static gw_rule_entry_t *s_rules;
static size_t s_rule_count;

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

static const char *strtab_at(const gw_auto_compiled_t *c, uint32_t off)
{
    if (!c) return "";
    if (off == 0) return "";
    if (off >= c->hdr.strings_size) return "";
    return c->strings + off;
}

static bool is_safe_id_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
}

static bool compiled_path_for_id(const char *id, char *out, size_t out_size)
{
    if (!id || !id[0] || !out || out_size < 16) return false;
    for (const char *p = id; *p; p++) {
        if (!is_safe_id_char(*p)) return false;
    }
    int n = snprintf(out, out_size, "/data/%s.gwar", id);
    return n > 0 && (size_t)n < out_size;
}

static void rules_cache_clear(void)
{
    for (size_t i = 0; i < s_rule_count; i++) {
        gw_auto_compiled_free(&s_rules[i].compiled);
    }
    free(s_rules);
    s_rules = NULL;
    s_rule_count = 0;
}

static ssize_t rules_find_idx(const char *id)
{
    if (!id || !id[0]) return -1;
    for (size_t i = 0; i < s_rule_count; i++) {
        if (strncmp(s_rules[i].id, id, sizeof(s_rules[i].id)) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void rules_remove_idx(size_t idx)
{
    if (idx >= s_rule_count) return;
    gw_auto_compiled_free(&s_rules[idx].compiled);
    for (size_t i = idx + 1; i < s_rule_count; i++) {
        s_rules[i - 1] = s_rules[i];
    }
    s_rule_count--;
    if (s_rule_count == 0) {
        free(s_rules);
        s_rules = NULL;
        return;
    }
    gw_rule_entry_t *shrunk = (gw_rule_entry_t *)realloc(s_rules, s_rule_count * sizeof(gw_rule_entry_t));
    if (shrunk) s_rules = shrunk;
}

static void rules_remove_id(const char *id)
{
    ssize_t idx = rules_find_idx(id);
    if (idx >= 0) {
        rules_remove_idx((size_t)idx);
    }
}

static esp_err_t rules_load_compiled_for_id(const char *id, gw_auto_compiled_t *out, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!id || !id[0] || !out) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }

    // Runtime executes ONLY compiled rules. Compilation happens on save in the store.
    char path[96];
    if (compiled_path_for_id(id, path, sizeof(path))) {
        esp_err_t rc = gw_auto_compiled_read_file(path, out);
        if (rc == ESP_OK) return ESP_OK;
    }

    set_err(err, err_size, "no compiled rule (save automation again)");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t rules_upsert_id(const char *id, bool enabled, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!id || !id[0]) {
        set_err(err, err_size, "missing id");
        return ESP_ERR_INVALID_ARG;
    }

    if (!enabled) {
        rules_remove_id(id);
        return ESP_OK;
    }

    // Free existing rule first to avoid peak heap usage during recompilation.
    rules_remove_id(id);

    gw_rule_entry_t *grown = (gw_rule_entry_t *)realloc(s_rules, (s_rule_count + 1) * sizeof(gw_rule_entry_t));
    if (!grown) {
        set_err(err, err_size, "no mem");
        return ESP_ERR_NO_MEM;
    }
    s_rules = grown;

    gw_rule_entry_t *re = &s_rules[s_rule_count];
    memset(re, 0, sizeof(*re));
    strlcpy(re->id, id, sizeof(re->id));

    esp_err_t rc = rules_load_compiled_for_id(id, &re->compiled, err, err_size);
    if (rc != ESP_OK) {
        // Roll back the entry if load failed.
        gw_auto_compiled_free(&re->compiled);
        memset(re, 0, sizeof(*re));
        return rc;
    }

    s_rule_count++;
    return ESP_OK;
}

static void publish_rules_fired(const gw_event_t *e, const char *automation_id)
{
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "automation=%s", automation_id ? automation_id : "");

    char payload[192];
    (void)snprintf(payload,
                   sizeof(payload),
                   "{\"automation_id\":\"%s\",\"event_id\":%u,\"event_type\":\"%s\"}",
                   automation_id ? automation_id : "",
                   (unsigned)(e ? e->id : 0),
                   e ? e->type : "");

    gw_event_bus_publish_ex("rules.fired", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg, payload);
}

static void publish_rules_action(const gw_event_t *e, const char *automation_id, size_t idx, bool ok, const char *err)
{
    char msg[96];
    (void)snprintf(msg,
                   sizeof(msg),
                   "automation=%s idx=%u ok=%u",
                   automation_id ? automation_id : "",
                   (unsigned)idx,
                   ok ? 1U : 0U);

    char payload[192];
    if (err && err[0] != '\0') {
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":false,\"err\":\"%s\"}",
                       automation_id ? automation_id : "",
                       (unsigned)idx,
                       err);
    } else {
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":%s}",
                       automation_id ? automation_id : "",
                       (unsigned)idx,
                       ok ? "true" : "false");
    }

    gw_event_bus_publish_ex("rules.action", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg, payload);
}

static void publish_cache_update(const char *id, const char *op, bool ok, const char *err)
{
    char msg[128];
    (void)snprintf(msg,
                   sizeof(msg),
                   "op=%s id=%s ok=%u rules=%u",
                   op ? op : "",
                   id ? id : "",
                   ok ? 1U : 0U,
                   (unsigned)s_rule_count);

    char payload[192];
    if (err && err[0]) {
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"op\":\"%s\",\"id\":\"%s\",\"ok\":false,\"rules\":%u,\"err\":\"%s\"}",
                       op ? op : "",
                       id ? id : "",
                       (unsigned)s_rule_count,
                       err);
    } else {
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"op\":\"%s\",\"id\":\"%s\",\"ok\":%s,\"rules\":%u}",
                       op ? op : "",
                       id ? id : "",
                       ok ? "true" : "false",
                       (unsigned)s_rule_count);
    }

    gw_event_bus_publish_ex("rules.cache", "rules", "", 0, msg, payload);
}

static bool parse_id_from_msg(const char *msg, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!msg) return false;

    // Support both: "<id>" and "id=<id> ..."
    const char *p = msg;
    if (strncmp(p, "id=", 3) == 0) {
        p += 3;
    }

    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n') {
        n++;
    }
    if (n == 0) return false;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool parse_enabled_event(const gw_event_t *e, char *out_id, size_t out_id_size, bool *out_enabled)
{
    if (!e || !out_id || out_id_size == 0 || !out_enabled) return false;
    out_id[0] = '\0';
    *out_enabled = false;

    if (e->payload_json[0]) {
        cJSON *p = cJSON_Parse(e->payload_json);
        if (p) {
            cJSON *id_j = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *en_j = cJSON_GetObjectItemCaseSensitive(p, "enabled");
            if (cJSON_IsString(id_j) && id_j->valuestring && id_j->valuestring[0] && cJSON_IsBool(en_j)) {
                strlcpy(out_id, id_j->valuestring, out_id_size);
                *out_enabled = cJSON_IsTrue(en_j);
                cJSON_Delete(p);
                return true;
            }
            cJSON_Delete(p);
        }
    }

    // Fallback: parse msg "id=<id> enabled=0/1"
    if (parse_id_from_msg(e->msg, out_id, out_id_size)) {
        const char *en = strstr(e->msg, "enabled=");
        if (en) {
            en += 8;
            *out_enabled = (*en == '1' || *en == 't' || *en == 'T');
        }
        return true;
    }

    return false;
}

typedef struct {
    uint8_t endpoint;
    bool has_endpoint;
    const char *cmd;
    bool has_cmd;
    uint16_t cluster_id;
    bool has_cluster;
    uint16_t attr_id;
    bool has_attr;
} event_payload_view_t;

static bool parse_u16_any_json(const cJSON *j, uint16_t *out)
{
    if (!out) return false;
    if (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 65535) {
        *out = (uint16_t)j->valuedouble;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        char *end = NULL;
        unsigned long v = strtoul(j->valuestring, &end, 0);
        if (end && *end == '\0' && v <= 65535UL) {
            *out = (uint16_t)v;
            return true;
        }
    }
    return false;
}

static void build_payload_view(const cJSON *payload, event_payload_view_t *out)
{
    if (!out) return;
    *out = (event_payload_view_t){0};
    if (!payload || !cJSON_IsObject(payload)) return;

    const cJSON *ep = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "endpoint");
    if (cJSON_IsNumber(ep) && ep->valuedouble >= 0 && ep->valuedouble <= 255) {
        out->endpoint = (uint8_t)ep->valuedouble;
        out->has_endpoint = true;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "cmd");
    if (cJSON_IsString(cmd) && cmd->valuestring) {
        out->cmd = cmd->valuestring;
        out->has_cmd = true;
    }

    uint16_t v = 0;
    const cJSON *cluster = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "cluster");
    if (parse_u16_any_json(cluster, &v)) {
        out->cluster_id = v;
        out->has_cluster = true;
    }

    const cJSON *attr = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "attr");
    if (parse_u16_any_json(attr, &v)) {
        out->attr_id = v;
        out->has_attr = true;
    }
}

static gw_auto_evt_type_t evt_type_from_event(const gw_event_t *e)
{
    if (!e || e->type[0] == '\0') return 0;
    if (strcmp(e->type, "zigbee.command") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(e->type, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(e->type, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(e->type, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static bool trigger_matches_compiled(const gw_auto_compiled_t *c,
                                    const gw_auto_bin_trigger_v2_t *t,
                                    gw_auto_evt_type_t evt_type,
                                    const gw_event_t *e,
                                    const event_payload_view_t *pv)
{
    if (!c || !t || !e) return false;
    if ((gw_auto_evt_type_t)t->event_type != evt_type) return false;

    if (t->device_uid_off) {
        const char *uid = strtab_at(c, t->device_uid_off);
        if (uid[0] && strcmp(uid, e->device_uid) != 0) return false;
    }

    if (t->endpoint) {
        if (!pv || !pv->has_endpoint) return false;
        if (pv->endpoint != t->endpoint) return false;
    }

    if (evt_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (t->cmd_off) {
            const char *cmd = strtab_at(c, t->cmd_off);
            if (!pv || !pv->has_cmd) return false;
            if (strcmp(cmd, pv->cmd) != 0) return false;
        }
        if (t->cluster_id) {
            if (!pv || !pv->has_cluster) return false;
            if (pv->cluster_id != t->cluster_id) return false;
        }
    } else if (evt_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (t->cluster_id) {
            if (!pv || !pv->has_cluster) return false;
            if (pv->cluster_id != t->cluster_id) return false;
        }
        if (t->attr_id) {
            if (!pv || !pv->has_attr) return false;
            if (pv->attr_id != t->attr_id) return false;
        }
    }

    return true;
}

static bool state_to_number_bool(const gw_state_item_t *s, double *out_n, bool *out_b)
{
    if (!s) return false;
    if (out_b) *out_b = false;
    if (out_n) *out_n = 0;

    switch (s->value_type) {
    case GW_STATE_VALUE_BOOL:
        if (out_b) *out_b = s->value_bool;
        if (out_n) *out_n = s->value_bool ? 1.0 : 0.0;
        return true;
    case GW_STATE_VALUE_F32:
        if (out_n) *out_n = (double)s->value_f32;
        if (out_b) *out_b = fabs((double)s->value_f32) > 0.000001;
        return true;
    case GW_STATE_VALUE_U32:
        if (out_n) *out_n = (double)s->value_u32;
        if (out_b) *out_b = s->value_u32 != 0;
        return true;
    case GW_STATE_VALUE_U64:
        if (out_n) *out_n = (double)s->value_u64;
        if (out_b) *out_b = s->value_u64 != 0;
        return true;
    default:
        return false;
    }
}

static bool conditions_pass_compiled(const gw_auto_compiled_t *c, const gw_auto_bin_condition_v2_t *conds, uint32_t cond_count)
{
    if (!c || cond_count == 0) return true;
    if (!conds) return true;

    for (uint32_t i = 0; i < cond_count; i++) {
        const gw_auto_bin_condition_v2_t *co = &conds[i];
        const char *uid_s = strtab_at(c, co->device_uid_off);
        const char *key = strtab_at(c, co->key_off);
        if (!uid_s[0] || !key[0]) {
            return false;
        }

        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_s, sizeof(uid.uid));

        gw_state_item_t st = {0};
        if (gw_state_store_get(&uid, key, &st) != ESP_OK) {
            return false;
        }

        double actual_n = 0;
        bool actual_b = false;
        if (!state_to_number_bool(&st, &actual_n, &actual_b)) {
            return false;
        }

        const gw_auto_op_t op = (gw_auto_op_t)co->op;
        if (co->val_type == GW_AUTO_VAL_BOOL) {
            const bool exp = (co->v.b != 0);
            const bool act = actual_b;
            if (op == GW_AUTO_OP_EQ && act != exp) return false;
            if (op == GW_AUTO_OP_NE && act == exp) return false;
            if (op == GW_AUTO_OP_GT && !(act > exp)) return false;
            if (op == GW_AUTO_OP_LT && !(act < exp)) return false;
            if (op == GW_AUTO_OP_GE && !(act >= exp)) return false;
            if (op == GW_AUTO_OP_LE && !(act <= exp)) return false;
        } else {
            const double exp = co->v.f64;
            const double act = actual_n;
            const double eps = 0.000001;
            if (op == GW_AUTO_OP_EQ && fabs(act - exp) > eps) return false;
            if (op == GW_AUTO_OP_NE && fabs(act - exp) <= eps) return false;
            if (op == GW_AUTO_OP_GT && !(act > exp)) return false;
            if (op == GW_AUTO_OP_LT && !(act < exp)) return false;
            if (op == GW_AUTO_OP_GE && !(act >= exp)) return false;
            if (op == GW_AUTO_OP_LE && !(act <= exp)) return false;
        }
    }

    return true;
}

static void process_event(const gw_event_t *e)
{
    if (!e || e->type[0] == '\0') return;

    // Avoid feedback loops from our own logs.
    if (strcmp(e->source, "rules") == 0) return;
    if (strncmp(e->type, "rules.", 6) == 0) return;

    // Control events: update compiled cache incrementally (avoid reloading all rules).
    if (strcmp(e->type, "automation_saved") == 0) {
        char id[GW_AUTOMATION_ID_MAX] = {0};
        if (parse_id_from_msg(e->msg, id, sizeof(id))) {
            char errbuf[96];
            esp_err_t rc = rules_upsert_id(id, true, errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                // If it was saved as disabled, ensure it's not active and don't spam logs.
                if (rc == ESP_ERR_INVALID_STATE && strcmp(errbuf, "disabled") == 0) {
                    rules_remove_id(id);
                    publish_cache_update(id, "saved", true, NULL);
                } else {
                    ESP_LOGW(TAG, "rule upsert failed (%s): %s", id, errbuf[0] ? errbuf : "err");
                    publish_cache_update(id, "saved", false, errbuf[0] ? errbuf : "err");
                }
            } else {
                publish_cache_update(id, "saved", true, NULL);
            }
        }
        return;
    }
    if (strcmp(e->type, "automation_removed") == 0) {
        char id[GW_AUTOMATION_ID_MAX] = {0};
        if (parse_id_from_msg(e->msg, id, sizeof(id))) {
            rules_remove_id(id);
            publish_cache_update(id, "removed", true, NULL);
        }
        return;
    }
    if (strcmp(e->type, "automation_enabled") == 0) {
        char id[GW_AUTOMATION_ID_MAX] = {0};
        bool enabled = false;
        if (parse_enabled_event(e, id, sizeof(id), &enabled) && id[0]) {
            char errbuf[96];
            esp_err_t rc = rules_upsert_id(id, enabled, errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "rule upsert failed (%s): %s", id, errbuf[0] ? errbuf : "err");
                publish_cache_update(id, enabled ? "enabled" : "disabled", false, errbuf[0] ? errbuf : "err");
            } else {
                publish_cache_update(id, enabled ? "enabled" : "disabled", true, NULL);
            }
        }
        return;
    }

    const gw_auto_evt_type_t evt_type = evt_type_from_event(e);
    if (!evt_type) return;

    // If no rules are loaded, log once in a while to make debugging easier.
    if (s_rule_count == 0) {
        static uint64_t s_last_no_rules_ms;
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (now_ms - s_last_no_rules_ms > 10000) {
            s_last_no_rules_ms = now_ms;
            publish_cache_update(NULL, "no_rules", false, "no rules loaded (save+enable automation)");
        }
        return;
    }

    cJSON *payload = NULL;
    if (e->payload_json[0] != '\0') {
        payload = cJSON_Parse(e->payload_json);
    }

    event_payload_view_t pv;
    build_payload_view(payload, &pv);

    for (size_t i = 0; i < s_rule_count; i++) {
        const gw_rule_entry_t *re = &s_rules[i];
        const gw_auto_compiled_t *c = &re->compiled;
        if (!c || c->hdr.magic != 0x52415747 || c->hdr.version != 2) continue;
        if (c->hdr.automation_count == 0 || !c->autos) continue;

        const gw_auto_bin_automation_v2_t *a0 = &c->autos[0];

        bool matched = false;
        for (uint32_t ti = 0; ti < a0->triggers_count; ti++) {
            const uint32_t idx = a0->triggers_index + ti;
            if (idx >= c->hdr.trigger_count_total) break;
            if (trigger_matches_compiled(c, &c->triggers[idx], evt_type, e, &pv)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        const gw_auto_bin_condition_v2_t *conds = NULL;
        if (a0->conditions_count) {
            if (a0->conditions_index >= c->hdr.condition_count_total) continue;
            conds = &c->conditions[a0->conditions_index];
        }
        if (!conditions_pass_compiled(c, conds, a0->conditions_count)) {
            continue;
        }

        publish_rules_fired(e, re->id);

        const gw_auto_bin_action_v2_t *acts = NULL;
        if (a0->actions_count) {
            if (a0->actions_index >= c->hdr.action_count_total) continue;
            acts = &c->actions[a0->actions_index];
        }

        for (uint32_t ai = 0; ai < a0->actions_count; ai++) {
            char errbuf[96];
            set_err(errbuf, sizeof(errbuf), NULL);
            esp_err_t rc = gw_action_exec_compiled(c, &acts[ai], errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                publish_rules_action(e, re->id, (size_t)ai, false, (errbuf[0] != '\0') ? errbuf : "exec failed");
                break;
            }
            publish_rules_action(e, re->id, (size_t)ai, true, NULL);
        }
    }

    if (payload) cJSON_Delete(payload);
}

static void rules_task(void *arg)
{
    (void)arg;
    gw_event_t e;
    while (true) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) {
            process_event(&e);
        }
    }
}

static void rules_event_listener(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!s_inited || !s_q || !event) {
        return;
    }
    (void)xQueueSend(s_q, event, 0);
}

esp_err_t gw_rules_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_q = xQueueCreate(16, sizeof(gw_event_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(rules_task, "rules", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_FAIL;
    }

    esp_err_t err = gw_event_bus_add_listener(rules_event_listener, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gw_event_bus_add_listener failed: %s", esp_err_to_name(err));
    }

    // Load enabled rules at startup without allocating N * json_max.
    {
        const size_t max_autos = 16;
        gw_automation_meta_t metas[max_autos];
        memset(metas, 0, sizeof(metas));
        size_t n = gw_automation_store_list_meta(metas, max_autos);
        for (size_t i = 0; i < n; i++) {
            if (!metas[i].enabled) continue;
            if (!metas[i].id[0]) continue;
            char errbuf[96];
            esp_err_t rc = rules_upsert_id(metas[i].id, true, errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "initial load failed (%s): %s", metas[i].id, errbuf[0] ? errbuf : "err");
            }
        }
        ESP_LOGI(TAG, "loaded compiled rules (count=%u)", (unsigned)s_rule_count);
    }

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized (compiled)");
    return ESP_OK;
}

esp_err_t gw_rules_handle_event(gw_event_id_t id, const void *data, size_t data_size)
{
    (void)id;
    if (!s_inited || !s_q || data == NULL || data_size != sizeof(gw_event_t)) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xQueueSend(s_q, data, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}
