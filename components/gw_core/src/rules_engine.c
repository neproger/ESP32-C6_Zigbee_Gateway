#include "gw_core/rules_engine.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_core/action_exec.h"
#include "gw_core/automation_store.h"
#include "gw_core/state_store.h"

static const char *TAG = "gw_rules";

static bool s_inited;
static QueueHandle_t s_q;
static TaskHandle_t s_task;

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

static bool json_truthy(const cJSON *j)
{
    if (cJSON_IsBool(j)) return cJSON_IsTrue(j);
    if (cJSON_IsNumber(j)) return fabs(j->valuedouble) > 0.000001;
    if (cJSON_IsString(j) && j->valuestring) return j->valuestring[0] != '\0';
    return false;
}

static bool parse_number_like(const cJSON *j, double *out)
{
    if (!out) return false;
    if (cJSON_IsNumber(j)) {
        *out = j->valuedouble;
        return true;
    }
    if (cJSON_IsBool(j)) {
        *out = cJSON_IsTrue(j) ? 1.0 : 0.0;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0] != '\0') {
        char *end = NULL;
        double v = strtod(j->valuestring, &end);
        if (end && *end == '\0') {
            *out = v;
            return true;
        }
        // Support hex like "0x0006"
        end = NULL;
        unsigned long uv = strtoul(j->valuestring, &end, 0);
        if (end && *end == '\0') {
            *out = (double)uv;
            return true;
        }
    }
    return false;
}

static const cJSON *payload_get_path(const cJSON *payload, const char *path)
{
    if (!payload || !cJSON_IsObject(payload) || !path || path[0] == '\0') {
        return NULL;
    }

    const cJSON *cur = payload;
    const char *p = path;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0) {
            return NULL;
        }

        char key[32];
        if (len >= sizeof(key)) {
            return NULL;
        }
        memcpy(key, p, len);
        key[len] = '\0';

        cur = cJSON_GetObjectItemCaseSensitive((cJSON *)cur, key);
        if (!cur) return NULL;

        if (!dot) break;
        if (!cJSON_IsObject(cur)) return NULL;
        p = dot + 1;
    }
    return cur;
}

static bool match_value(const cJSON *expected, const char *actual_s, double actual_n, bool has_actual_s, bool has_actual_n, bool actual_bool, bool has_actual_bool)
{
    if (!expected) return false;

    if (cJSON_IsString(expected) && expected->valuestring) {
        if (has_actual_s) {
            return strcmp(expected->valuestring, actual_s) == 0;
        }
        if (has_actual_n) {
            double exp_n = 0;
            cJSON *tmp = cJSON_CreateString(expected->valuestring);
            bool ok = parse_number_like(tmp, &exp_n);
            cJSON_Delete(tmp);
            if (ok) {
                return fabs(exp_n - actual_n) < 0.000001;
            }
        }
        if (has_actual_bool) {
            if (strcmp(expected->valuestring, "true") == 0) return actual_bool;
            if (strcmp(expected->valuestring, "false") == 0) return !actual_bool;
        }
        return false;
    }

    if (cJSON_IsNumber(expected)) {
        double exp_n = expected->valuedouble;
        if (has_actual_n) return fabs(exp_n - actual_n) < 0.000001;
        if (has_actual_s) {
            double act_n = 0;
            cJSON *tmp = cJSON_CreateString(actual_s);
            bool ok = parse_number_like(tmp, &act_n);
            cJSON_Delete(tmp);
            if (ok) return fabs(exp_n - act_n) < 0.000001;
        }
        if (has_actual_bool) return fabs(exp_n - (actual_bool ? 1.0 : 0.0)) < 0.000001;
        return false;
    }

    if (cJSON_IsBool(expected)) {
        bool exp_b = cJSON_IsTrue(expected);
        if (has_actual_bool) return exp_b == actual_bool;
        if (has_actual_n) return exp_b == (fabs(actual_n) > 0.000001);
        if (has_actual_s) {
            if (strcmp(actual_s, "true") == 0) return exp_b;
            if (strcmp(actual_s, "false") == 0) return !exp_b;
        }
        return false;
    }

    return false;
}

static bool match_event_field(const char *key, const cJSON *expected, const gw_event_t *e, const cJSON *payload)
{
    if (!key || !expected || !e) return false;

    if (strncmp(key, "payload.", 8) == 0) {
        const cJSON *v = payload_get_path(payload, key + 8);
        if (!v) return false;

        if (cJSON_IsString(v) && v->valuestring) {
            return match_value(expected, v->valuestring, 0, true, false, false, false);
        }
        if (cJSON_IsNumber(v)) {
            return match_value(expected, NULL, v->valuedouble, false, true, false, false);
        }
        if (cJSON_IsBool(v)) {
            return match_value(expected, NULL, 0, false, false, cJSON_IsTrue(v), true);
        }
        return false;
    }

    if (strcmp(key, "type") == 0) {
        return match_value(expected, e->type, 0, true, false, false, false);
    }
    if (strcmp(key, "source") == 0) {
        return match_value(expected, e->source, 0, true, false, false, false);
    }
    if (strcmp(key, "device_uid") == 0) {
        return match_value(expected, e->device_uid, 0, true, false, false, false);
    }
    if (strcmp(key, "short_addr") == 0) {
        return match_value(expected, NULL, (double)e->short_addr, false, true, false, false);
    }

    return false;
}

static bool trigger_matches(const cJSON *trigger, const gw_event_t *e, const cJSON *payload)
{
    if (!cJSON_IsObject(trigger) || !e) return false;

    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive((cJSON *)trigger, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring || strcmp(type_j->valuestring, "event") != 0) {
        return false;
    }

    const cJSON *event_type_j = cJSON_GetObjectItemCaseSensitive((cJSON *)trigger, "event_type");
    if (!cJSON_IsString(event_type_j) || !event_type_j->valuestring) {
        return false;
    }
    if (strcmp(event_type_j->valuestring, e->type) != 0) {
        return false;
    }

    const cJSON *match = cJSON_GetObjectItemCaseSensitive((cJSON *)trigger, "match");
    if (!match) return true;
    if (!cJSON_IsObject(match)) return false;

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, (cJSON *)match)
    {
        if (!it || !it->string) return false;
        if (!match_event_field(it->string, it, e, payload)) {
            return false;
        }
    }
    return true;
}

static bool condition_passes(const cJSON *cond, const gw_event_t *e)
{
    (void)e;
    if (!cJSON_IsObject(cond)) return false;

    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive((cJSON *)cond, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring || strcmp(type_j->valuestring, "state") != 0) {
        return false;
    }

    const cJSON *op_j = cJSON_GetObjectItemCaseSensitive((cJSON *)cond, "op");
    const cJSON *ref_j = cJSON_GetObjectItemCaseSensitive((cJSON *)cond, "ref");
    const cJSON *value_j = cJSON_GetObjectItemCaseSensitive((cJSON *)cond, "value");
    if (!cJSON_IsString(op_j) || !op_j->valuestring) return false;
    if (!cJSON_IsObject(ref_j)) return false;
    if (!value_j) return false;

    const cJSON *uid_j = cJSON_GetObjectItemCaseSensitive((cJSON *)ref_j, "device_uid");
    const cJSON *key_j = cJSON_GetObjectItemCaseSensitive((cJSON *)ref_j, "key");
    if (!cJSON_IsString(uid_j) || !uid_j->valuestring || uid_j->valuestring[0] == '\0') return false;
    if (!cJSON_IsString(key_j) || !key_j->valuestring || key_j->valuestring[0] == '\0') return false;

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_j->valuestring, sizeof(uid.uid));

    gw_state_item_t item = {0};
    if (gw_state_store_get(&uid, key_j->valuestring, &item) != ESP_OK) {
        return false;
    }

    double actual = 0;
    switch (item.value_type) {
    case GW_STATE_VALUE_BOOL:
        actual = item.value_bool ? 1.0 : 0.0;
        break;
    case GW_STATE_VALUE_F32:
        actual = (double)item.value_f32;
        break;
    case GW_STATE_VALUE_U32:
        actual = (double)item.value_u32;
        break;
    case GW_STATE_VALUE_U64:
        actual = (double)item.value_u64;
        break;
    default:
        return false;
    }

    double expected = 0;
    if (!parse_number_like(value_j, &expected)) {
        expected = json_truthy(value_j) ? 1.0 : 0.0;
    }

    const char *op = op_j->valuestring;
    if (strcmp(op, "==") == 0) return fabs(actual - expected) < 0.000001;
    if (strcmp(op, "!=") == 0) return fabs(actual - expected) >= 0.000001;
    if (strcmp(op, ">") == 0) return actual > expected;
    if (strcmp(op, "<") == 0) return actual < expected;
    if (strcmp(op, ">=") == 0) return actual >= expected;
    if (strcmp(op, "<=") == 0) return actual <= expected;

    return false;
}

static bool conditions_pass(const cJSON *conds, const gw_event_t *e)
{
    if (!conds) return true;
    if (!cJSON_IsArray(conds)) return false;

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, (cJSON *)conds)
    {
        if (!condition_passes(it, e)) {
            return false;
        }
    }
    return true;
}

static void publish_rules_fired(const gw_event_t *e, const gw_automation_t *a)
{
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "automation=%s", a ? a->id : "");

    char payload[192];
    (void)snprintf(payload,
                   sizeof(payload),
                   "{\"automation_id\":\"%s\",\"event_id\":%u,\"event_type\":\"%s\"}",
                   a ? a->id : "",
                   (unsigned)(e ? e->id : 0),
                   e ? e->type : "");

    gw_event_bus_publish_ex("rules.fired", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg, payload);
}

static void publish_rules_action(const gw_event_t *e, const gw_automation_t *a, size_t idx, bool ok, const char *err)
{
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "automation=%s idx=%u ok=%u", a ? a->id : "", (unsigned)idx, ok ? 1U : 0U);

    char payload[192];
    if (err && err[0] != '\0') {
        // Keep payload small; err is bounded by caller.
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":false,\"err\":\"%s\"}",
                       a ? a->id : "",
                       (unsigned)idx,
                       err);
    } else {
        (void)snprintf(payload,
                       sizeof(payload),
                       "{\"automation_id\":\"%s\",\"idx\":%u,\"ok\":%s}",
                       a ? a->id : "",
                       (unsigned)idx,
                       ok ? "true" : "false");
    }

    gw_event_bus_publish_ex("rules.action", "rules", e ? e->device_uid : "", e ? e->short_addr : 0, msg, payload);
}

static void process_event(const gw_event_t *e)
{
    if (!e || e->type[0] == '\0') return;

    // Avoid feedback loops from our own logs.
    if (strcmp(e->source, "rules") == 0) return;
    if (strncmp(e->type, "rules.", 6) == 0) return;

    const size_t max_autos = 16;
    gw_automation_t *autos = (gw_automation_t *)calloc(max_autos, sizeof(gw_automation_t));
    if (!autos) {
        return;
    }

    size_t count = gw_automation_store_list(autos, max_autos);

    cJSON *payload = NULL;
    if (e->payload_json[0] != '\0') {
        payload = cJSON_Parse(e->payload_json);
    }

    for (size_t i = 0; i < count; i++) {
        gw_automation_t *a = &autos[i];
        if (!a->enabled) continue;
        if (a->json[0] == '\0') continue;

        cJSON *root = cJSON_Parse(a->json);
        if (!root) continue;

        cJSON *triggers = cJSON_GetObjectItemCaseSensitive(root, "triggers");
        cJSON *conditions = cJSON_GetObjectItemCaseSensitive(root, "conditions");
        cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");

        bool matched = false;
        if (cJSON_IsArray(triggers)) {
            cJSON *t = NULL;
            cJSON_ArrayForEach(t, triggers)
            {
                if (trigger_matches(t, e, payload)) {
                    matched = true;
                    break;
                }
            }
        }

        if (!matched) {
            cJSON_Delete(root);
            continue;
        }

        if (!conditions_pass(conditions, e)) {
            cJSON_Delete(root);
            continue;
        }

        publish_rules_fired(e, a);

        if (cJSON_IsArray(actions)) {
            size_t action_idx = 0;
            cJSON *act = NULL;
            cJSON_ArrayForEach(act, actions)
            {
                if (!cJSON_IsObject(act)) {
                    publish_rules_action(e, a, action_idx, false, "action not object");
                    break;
                }
                char errbuf[96];
                set_err(errbuf, sizeof(errbuf), NULL);
                esp_err_t err = gw_action_exec(act, errbuf, sizeof(errbuf));
                if (err != ESP_OK) {
                    publish_rules_action(e, a, action_idx, false, (errbuf[0] != '\0') ? errbuf : "exec failed");
                    break;
                }
                publish_rules_action(e, a, action_idx, true, NULL);
                action_idx++;
            }
        }

        cJSON_Delete(root);
    }

    if (payload) {
        cJSON_Delete(payload);
    }
    free(autos);
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

    // Keep callback fast and non-blocking: best-effort enqueue.
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
        // Task/queue are still useful, but without listener it won't run.
        ESP_LOGW(TAG, "gw_event_bus_add_listener failed: %s", esp_err_to_name(err));
    }

    s_inited = true;
    ESP_LOGI(TAG, "rules engine initialized (MVP)");
    return ESP_OK;
}

esp_err_t gw_rules_handle_event(gw_event_id_t id, const void *data, size_t data_size)
{
    (void)id;
    if (!s_inited || !s_q || data == NULL || data_size != sizeof(gw_event_t)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Best-effort enqueue from explicit callers.
    return (xQueueSend(s_q, data, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

