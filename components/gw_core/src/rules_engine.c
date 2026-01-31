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
#include "gw_core/automation_store.h"
#include "gw_core/state_store.h"
#include "gw_core/types.h"

static const char *TAG = "gw_rules";

#define GW_AUTOMATION_CAP 32

static bool s_inited;
static QueueHandle_t s_q;
static TaskHandle_t s_task;

static const char *strtab_at(const gw_automation_entry_t *entry, uint32_t off)
{
    if (!entry) return "";
    if (off == 0) return "";
    if (off >= entry->string_table_size) return "";
    return entry->string_table + off;
}

static void publish_rules_fired(const gw_event_t *e, const char *automation_id)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"automation_id\":\"%s\"}", automation_id ? automation_id : "");
    gw_event_bus_publish("rules.fired", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg);
}

static void publish_rules_action(const char *automation_id, size_t idx, bool ok, const char *err)
{
    char msg[192];
    if (err) {
        snprintf(msg, sizeof(msg), "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":false,\"err\":\"%s\"}", automation_id, (unsigned)idx, err);
    } else {
        snprintf(msg, sizeof(msg), "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":true}", automation_id, (unsigned)idx);
    }
    gw_event_bus_publish("rules.action", "rules", "", 0, msg);
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
    if (cJSON_IsNumber(j)) { *out = (uint16_t)j->valuedouble; return true; }
    if (cJSON_IsString(j)) { *out = (uint16_t)strtoul(j->valuestring, NULL, 0); return true; }
    return false;
}

static void build_payload_view(const cJSON *payload, event_payload_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!payload) return;
    const cJSON *ep = cJSON_GetObjectItem(payload, "endpoint");
    if (cJSON_IsNumber(ep)) { out->endpoint = (uint8_t)ep->valuedouble; out->has_endpoint = true; }
    const cJSON *cmd = cJSON_GetObjectItem(payload, "cmd");
    if (cJSON_IsString(cmd)) { out->cmd = cmd->valuestring; out->has_cmd = true; }
    if (parse_u16_any_json(cJSON_GetObjectItem(payload, "cluster"), &out->cluster_id)) { out->has_cluster = true; }
    if (parse_u16_any_json(cJSON_GetObjectItem(payload, "attr"), &out->attr_id)) { out->has_attr = true; }
}

static gw_auto_evt_type_t evt_type_from_event(const gw_event_t *e)
{
    if (strcmp(e->type, "zigbee.command") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(e->type, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(e->type, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(e->type, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static bool trigger_matches(const gw_automation_entry_t *entry, const gw_auto_bin_trigger_v2_t *t, gw_auto_evt_type_t evt_type, const gw_event_t *e, const event_payload_view_t *pv)
{
    if (t->event_type != evt_type) return false;
    if (t->device_uid_off && strcmp(strtab_at(entry, t->device_uid_off), e->device_uid) != 0) return false;
    if (t->endpoint && (!pv->has_endpoint || pv->endpoint != t->endpoint)) return false;

    if (evt_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (t->cmd_off && (!pv->has_cmd || strcmp(strtab_at(entry, t->cmd_off), pv->cmd) != 0)) return false;
        if (t->cluster_id && (!pv->has_cluster || pv->cluster_id != t->cluster_id)) return false;
    } else if (evt_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (t->cluster_id && (!pv->has_cluster || pv->cluster_id != t->cluster_id)) return false;
        if (t->attr_id && (!pv->has_attr || pv->attr_id != t->attr_id)) return false;
    }
    return true;
}

static bool state_to_number_bool(const gw_state_item_t *s, double *out_n, bool *out_b)
{
    if (!s) return false;
    switch (s->value_type) {
    case GW_STATE_VALUE_BOOL: *out_n = s->value_bool ? 1.0 : 0.0; *out_b = s->value_bool; return true;
    case GW_STATE_VALUE_F32:  *out_n = s->value_f32; *out_b = fabs(s->value_f32) > 1e-6; return true;
    case GW_STATE_VALUE_U32:  *out_n = s->value_u32; *out_b = s->value_u32 != 0; return true;
    case GW_STATE_VALUE_U64:  *out_n = s->value_u64; *out_b = s->value_u64 != 0; return true;
    default: return false;
    }
}

static bool conditions_pass(const gw_automation_entry_t *entry)
{
    if (entry->conditions_count == 0) return true;

    for (uint8_t i = 0; i < entry->conditions_count; i++) {
        const gw_auto_bin_condition_v2_t *co = &entry->conditions[i];
        const char *uid_s = strtab_at(entry, co->device_uid_off);
        const char *key = strtab_at(entry, co->key_off);
        if (!uid_s[0] || !key[0]) return false;

        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_s, sizeof(uid.uid));
        gw_state_item_t st = {0};
        if (gw_state_store_get(&uid, key, &st) != ESP_OK) return false;

        double actual_n = 0;
        bool actual_b = false;
        if (!state_to_number_bool(&st, &actual_n, &actual_b)) return false;

        const gw_auto_op_t op = (gw_auto_op_t)co->op;
        if (co->val_type == GW_AUTO_VAL_BOOL) {
            bool exp = co->v.b != 0;
            if ((op == GW_AUTO_OP_EQ && actual_b != exp) || (op == GW_AUTO_OP_NE && actual_b == exp)) return false;
        } else {
            double exp = co->v.f64;
            double act = actual_n;
            if ((op == GW_AUTO_OP_EQ && fabs(act - exp) > 1e-6) || (op == GW_AUTO_OP_NE && fabs(act - exp) < 1e-6) ||
                (op == GW_AUTO_OP_GT && act <= exp) || (op == GW_AUTO_OP_LT && act >= exp) ||
                (op == GW_AUTO_OP_GE && act < exp) || (op == GW_AUTO_OP_LE && act > exp)) return false;
        }
    }
    return true;
}

static void process_event(const gw_event_t *e)
{
    if (!e || !e->type[0] || strcmp(e->source, "rules") == 0) return;

    const gw_auto_evt_type_t evt_type = evt_type_from_event(e);
    if (!evt_type) return;
    
    gw_automation_entry_t *all_autos = (gw_automation_entry_t *)calloc(GW_AUTOMATION_CAP, sizeof(gw_automation_entry_t));
    if (!all_autos) {
        ESP_LOGE(TAG, "Failed to allocate memory for automations");
        return;
    }

    size_t auto_count = gw_automation_store_list(all_autos, GW_AUTOMATION_CAP);
    if (auto_count == 0) {
        free(all_autos);
        return;
    }

    cJSON *payload = e->payload_json[0] ? cJSON_Parse(e->payload_json) : NULL;
    event_payload_view_t pv;
    build_payload_view(payload, &pv);

    for (size_t i = 0; i < auto_count; i++) {
        const gw_automation_entry_t *entry = &all_autos[i];
        if (!entry->enabled) continue;

        bool matched = false;
        for (uint8_t ti = 0; ti < entry->triggers_count; ti++) {
            if (trigger_matches(entry, &entry->triggers[ti], evt_type, e, &pv)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        if (!conditions_pass(entry)) continue;

        publish_rules_fired(e, entry->id);

        for (uint8_t ai = 0; ai < entry->actions_count; ai++) {
            char errbuf[96] = {0};
            // This function needs to be adapted or wrapped to work with gw_automation_entry_t
            // For now, let's assume a wrapper exists. We will need to create it.
            // esp_err_t rc = gw_action_exec_compiled(c, &acts[ai], errbuf, sizeof(errbuf));
            // Let's create a temporary compatible structure for gw_action_exec_compiled
            gw_auto_compiled_t temp_compiled = {
                .strings = (char*)entry->string_table,
                .hdr.strings_size = entry->string_table_size
            };

            esp_err_t rc = gw_action_exec_compiled(&temp_compiled, &entry->actions[ai], errbuf, sizeof(errbuf));
            if (rc != ESP_OK) {
                publish_rules_action(entry->id, ai, false, errbuf[0] ? errbuf : "exec failed");
                break; // Stop actions on first failure for this rule
            }
            publish_rules_action(entry->id, ai, true, NULL);
        }
    }

    if (payload) cJSON_Delete(payload);
    free(all_autos);
}

static void rules_task(void *arg)
{
    gw_event_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) {
            process_event(&e);
        }
    }
}

static void rules_event_listener(const gw_event_t *event, void *user_ctx)
{
    if (s_inited && s_q && event) {
        if (xQueueSend(s_q, event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "rules event queue overflow");
        }
    }
}

esp_err_t gw_rules_init(void)
{
    if (s_inited) return ESP_OK;

    s_q = xQueueCreate(16, sizeof(gw_event_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    if (xTaskCreate(rules_task, "rules", 4096, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_q);
        return ESP_FAIL;
    }

    gw_event_bus_add_listener(rules_event_listener, NULL);

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized");
    return ESP_OK;
}
