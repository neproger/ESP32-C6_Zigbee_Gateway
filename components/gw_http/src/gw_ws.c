#include "gw_http/gw_ws.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "gw_core/device_registry.h"
#include "gw_core/automation_store.h"
#include "gw_core/event_bus.h"
#include "gw_zigbee/gw_zigbee.h"

static const char *TAG = "gw_ws";

typedef struct {
    int fd;
    bool subscribed_events;
} gw_ws_client_t;

static httpd_handle_t s_server;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;

#define GW_WS_MAX_CLIENTS 8
static gw_ws_client_t s_clients[GW_WS_MAX_CLIENTS];

static void ws_client_remove_fd(int fd);

static void ws_transfer_done_cb(esp_err_t err, int socket, void *arg)
{
    (void)err;
    (void)socket;
    free(arg);
}

static esp_err_t ws_send_json_async(int fd, const char *json)
{
    if (!s_server || !json) {
        return ESP_ERR_INVALID_STATE;
    }

    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_client_remove_fd(fd);
        return ESP_ERR_INVALID_STATE;
    }

    size_t n = strlen(json);
    char *copy = (char *)malloc(n + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, json, n + 1);

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)copy,
        .len = n,
    };

    esp_err_t err = httpd_ws_send_data_async(s_server, fd, &frame, ws_transfer_done_cb, copy);
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}

static void ws_send_hello(int fd)
{
    char buf[256];
    int n = snprintf(buf,
                     sizeof(buf),
                     "{\"t\":\"hello\",\"proto\":\"gw-ws-1\",\"caps\":{\"events\":true,\"req\":true},\"event_last_id\":%u}",
                     (unsigned)gw_event_bus_last_id());
    if (n > 0 && (size_t)n < sizeof(buf)) {
        (void)ws_send_json_async(fd, buf);
    }
}

static void ws_client_remove_fd(int fd)
{
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = (gw_ws_client_t){0};
            break;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
}

static bool ws_client_add_fd(int fd)
{
    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            portEXIT_CRITICAL(&s_client_lock);
            return true;
        }
    }
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == 0) {
            s_clients[i].fd = fd;
            s_clients[i].subscribed_events = false;
            portEXIT_CRITICAL(&s_client_lock);
            return true;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);
    return false;
}

static void ws_send_events_since(int fd, uint32_t since, size_t limit)
{
    if (limit < 1) {
        return;
    }
    if (limit > 128) {
        limit = 128;
    }

    gw_event_t *events = (gw_event_t *)calloc(limit, sizeof(gw_event_t));
    if (!events) {
        return;
    }

    uint32_t last_id = 0;
    size_t count = gw_event_bus_list_since(since, events, limit, &last_id);
    for (size_t i = 0; i < count; i++) {
        const gw_event_t *e = &events[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            continue;
        }
        cJSON_AddStringToObject(o, "t", "event");
        cJSON_AddNumberToObject(o, "id", (double)e->id);
        cJSON_AddNumberToObject(o, "ts_ms", (double)e->ts_ms);
        cJSON_AddStringToObject(o, "type", e->type);
        cJSON_AddStringToObject(o, "source", e->source);
        cJSON_AddStringToObject(o, "device_uid", e->device_uid);
        cJSON_AddNumberToObject(o, "short_addr", (double)e->short_addr);
        cJSON_AddStringToObject(o, "msg", e->msg);

        // If msg looks like JSON, also expose it as payload for normalized events.
        if (e->msg[0] == '{' || e->msg[0] == '[') {
            cJSON *p = cJSON_Parse(e->msg);
            if (p) {
                cJSON_AddItemToObject(o, "payload", p);
            }
        }

        char *s = cJSON_PrintUnformatted(o);
        if (s) {
            (void)ws_send_json_async(fd, s);
            cJSON_free(s);
        }
        cJSON_Delete(o);
    }

    free(events);
}

static void ws_publish_event_to_clients(const gw_event_t *e, void *user_ctx)
{
    (void)user_ctx;
    if (!s_server || !e) {
        return;
    }

    int fds[GW_WS_MAX_CLIENTS];
    size_t fd_count = 0;

    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd != 0 && s_clients[i].subscribed_events) {
            fds[fd_count++] = s_clients[i].fd;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }

    cJSON_AddStringToObject(o, "t", "event");
    cJSON_AddNumberToObject(o, "id", (double)e->id);
    cJSON_AddNumberToObject(o, "ts_ms", (double)e->ts_ms);
    cJSON_AddStringToObject(o, "type", e->type);
    cJSON_AddStringToObject(o, "source", e->source);
    cJSON_AddStringToObject(o, "device_uid", e->device_uid);
    cJSON_AddNumberToObject(o, "short_addr", (double)e->short_addr);
    cJSON_AddStringToObject(o, "msg", e->msg);

    if (e->msg[0] == '{' || e->msg[0] == '[') {
        cJSON *p = cJSON_Parse(e->msg);
        if (p) {
            cJSON_AddItemToObject(o, "payload", p);
        }
    }

    char *s = cJSON_PrintUnformatted(o);
    if (!s) {
        cJSON_Delete(o);
        return;
    }

    for (size_t i = 0; i < fd_count; i++) {
        int fd = fds[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            ws_client_remove_fd(fd);
            continue;
        }
        (void)ws_send_json_async(fd, s);
    }

    cJSON_free(s);
    cJSON_Delete(o);
}

static void ws_send_rsp(int fd, cJSON *id, bool ok, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "t", "rsp");
    if (id) {
        cJSON_AddItemToObject(o, "id", cJSON_Duplicate(id, 1));
    }
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok && err) {
        cJSON_AddStringToObject(o, "err", err);
    }
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        (void)ws_send_json_async(fd, s);
        cJSON_free(s);
    }
    cJSON_Delete(o);
}

static void ws_handle_req(int fd, cJSON *root)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "m");
    if (!cJSON_IsString(m) || !m->valuestring) {
        ws_send_rsp(fd, id, false, "missing m");
        return;
    }

    if (strcmp(m->valuestring, "events.list") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        uint32_t since = 0;
        size_t limit = 64;
        if (cJSON_IsObject(p)) {
            cJSON *since_j = cJSON_GetObjectItemCaseSensitive(p, "since");
            cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(p, "limit");
            if (cJSON_IsNumber(since_j) && since_j->valuedouble >= 0) {
                since = (uint32_t)since_j->valuedouble;
            }
            if (cJSON_IsNumber(limit_j) && limit_j->valuedouble >= 1 && limit_j->valuedouble <= 128) {
                limit = (size_t)limit_j->valuedouble;
            }
        }

        gw_event_t *events = (gw_event_t *)calloc(limit, sizeof(gw_event_t));
        if (!events) {
            ws_send_rsp(fd, id, false, "no mem");
            return;
        }

        uint32_t last_id = 0;
        size_t count = gw_event_bus_list_since(since, events, limit, &last_id);

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "rsp");
        if (id) cJSON_AddItemToObject(o, "id", cJSON_Duplicate(id, 1));
        cJSON_AddBoolToObject(o, "ok", true);

        cJSON *res = cJSON_AddObjectToObject(o, "res");
        cJSON_AddNumberToObject(res, "last_id", (double)last_id);
        cJSON *arr = cJSON_AddArrayToObject(res, "events");
        for (size_t i = 0; i < count; i++) {
            const gw_event_t *e = &events[i];
            cJSON *je = cJSON_CreateObject();
            cJSON_AddNumberToObject(je, "id", (double)e->id);
            cJSON_AddNumberToObject(je, "ts_ms", (double)e->ts_ms);
            cJSON_AddStringToObject(je, "type", e->type);
            cJSON_AddStringToObject(je, "source", e->source);
            cJSON_AddStringToObject(je, "device_uid", e->device_uid);
            cJSON_AddNumberToObject(je, "short_addr", (double)e->short_addr);
            cJSON_AddStringToObject(je, "msg", e->msg);
            cJSON_AddItemToArray(arr, je);
        }

        char *s = cJSON_PrintUnformatted(o);
        if (s) {
            (void)ws_send_json_async(fd, s);
            cJSON_free(s);
        }
        cJSON_Delete(o);
        free(events);
        return;
    }

    if (strcmp(m->valuestring, "automations.list") == 0) {
        gw_automation_t autos[16] = {0};
        size_t count = gw_automation_store_list(autos, 16);

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "rsp");
        if (id) cJSON_AddItemToObject(o, "id", cJSON_Duplicate(id, 1));
        cJSON_AddBoolToObject(o, "ok", true);

        cJSON *res = cJSON_AddObjectToObject(o, "res");
        cJSON *arr = cJSON_AddArrayToObject(res, "automations");
        for (size_t i = 0; i < count; i++) {
            const gw_automation_t *a = &autos[i];
            cJSON *ja = cJSON_CreateObject();
            cJSON_AddStringToObject(ja, "id", a->id);
            cJSON_AddStringToObject(ja, "name", a->name);
            cJSON_AddBoolToObject(ja, "enabled", a->enabled);
            cJSON_AddStringToObject(ja, "json", a->json);
            cJSON_AddItemToArray(arr, ja);
        }

        char *s = cJSON_PrintUnformatted(o);
        if (s) {
            (void)ws_send_json_async(fd, s);
            cJSON_free(s);
        }
        cJSON_Delete(o);
        return;
    }

    if (strcmp(m->valuestring, "automations.put") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        cJSON *id_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "id") : NULL;
        cJSON *name_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "name") : NULL;
        cJSON *enabled_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "enabled") : NULL;
        cJSON *json_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "json") : NULL;
        if (!cJSON_IsString(id_j) || !id_j->valuestring || id_j->valuestring[0] == '\0') {
            ws_send_rsp(fd, id, false, "missing id");
            return;
        }
        if (!cJSON_IsString(name_j) || !name_j->valuestring) {
            ws_send_rsp(fd, id, false, "missing name");
            return;
        }
        if (!cJSON_IsString(json_j) || !json_j->valuestring) {
            ws_send_rsp(fd, id, false, "missing json");
            return;
        }

        gw_automation_t a = {0};
        strlcpy(a.id, id_j->valuestring, sizeof(a.id));
        strlcpy(a.name, name_j->valuestring, sizeof(a.name));
        strlcpy(a.json, json_j->valuestring, sizeof(a.json));
        a.enabled = cJSON_IsBool(enabled_j) ? cJSON_IsTrue(enabled_j) : true;

        esp_err_t err = gw_automation_store_put(&a);
        if (err != ESP_OK) {
            ws_send_rsp(fd, id, false, "store failed");
            return;
        }
        gw_event_bus_publish("automation_saved", "ws", "", 0, a.id);
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    if (strcmp(m->valuestring, "automations.remove") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        cJSON *id_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "id") : NULL;
        if (!cJSON_IsString(id_j) || !id_j->valuestring || id_j->valuestring[0] == '\0') {
            ws_send_rsp(fd, id, false, "missing id");
            return;
        }
        esp_err_t err = gw_automation_store_remove(id_j->valuestring);
        if (err != ESP_OK) {
            ws_send_rsp(fd, id, false, "not found");
            return;
        }
        gw_event_bus_publish("automation_removed", "ws", "", 0, id_j->valuestring);
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    if (strcmp(m->valuestring, "automations.set_enabled") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        cJSON *id_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "id") : NULL;
        cJSON *enabled_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "enabled") : NULL;
        if (!cJSON_IsString(id_j) || !id_j->valuestring || id_j->valuestring[0] == '\0') {
            ws_send_rsp(fd, id, false, "missing id");
            return;
        }
        if (!cJSON_IsBool(enabled_j)) {
            ws_send_rsp(fd, id, false, "missing enabled");
            return;
        }
        esp_err_t err = gw_automation_store_set_enabled(id_j->valuestring, cJSON_IsTrue(enabled_j));
        if (err != ESP_OK) {
            ws_send_rsp(fd, id, false, "not found");
            return;
        }
        gw_event_bus_publish("automation_enabled", "ws", "", 0, cJSON_IsTrue(enabled_j) ? "1" : "0");
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    if (strcmp(m->valuestring, "network.permit_join") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        int seconds = 180;
        if (cJSON_IsObject(p)) {
            cJSON *sec = cJSON_GetObjectItemCaseSensitive(p, "seconds");
            if (cJSON_IsNumber(sec) && sec->valueint > 0 && sec->valueint <= 255) {
                seconds = sec->valueint;
            }
        }

        esp_err_t err = gw_zigbee_permit_join((uint8_t)seconds);
        if (err != ESP_OK) {
            ws_send_rsp(fd, id, false, "permit_join failed");
            return;
        }
        char emsg[48];
        (void)snprintf(emsg, sizeof(emsg), "seconds=%u", (unsigned)seconds);
        gw_event_bus_publish("api_permit_join", "ws", "", 0, emsg);
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    if (strcmp(m->valuestring, "devices.remove") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        cJSON *uid_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "uid") : NULL;
        cJSON *kick_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "kick") : NULL;
        if (!cJSON_IsString(uid_j) || !uid_j->valuestring || uid_j->valuestring[0] == '\0') {
            ws_send_rsp(fd, id, false, "missing uid");
            return;
        }

        bool kick = cJSON_IsBool(kick_j) ? cJSON_IsTrue(kick_j) : false;
        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_j->valuestring, sizeof(uid.uid));

        uint16_t short_addr = 0;
        if (kick) {
            gw_device_t d = {0};
            esp_err_t gerr = gw_device_registry_get(&uid, &d);
            if (gerr != ESP_OK) {
                ws_send_rsp(fd, id, false, "device not found");
                return;
            }
            short_addr = d.short_addr;
            esp_err_t lerr = gw_zigbee_device_leave(&uid, short_addr, false);
            if (lerr != ESP_OK) {
                ws_send_rsp(fd, id, false, "leave failed");
                return;
            }
            char msg[64];
            (void)snprintf(msg, sizeof(msg), "uid=%s short=0x%04x", uid.uid, (unsigned)short_addr);
            gw_event_bus_publish("api_device_kick", "ws", uid.uid, short_addr, msg);
        }

        esp_err_t rm = gw_device_registry_remove(&uid);
        if (rm != ESP_OK) {
            ws_send_rsp(fd, id, false, "device not found");
            return;
        }
        gw_event_bus_publish("api_device_removed", "ws", uid.uid, short_addr, kick ? "kick=1" : "kick=0");
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    if (strcmp(m->valuestring, "devices.set_name") == 0) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        cJSON *uid_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "uid") : NULL;
        cJSON *name_j = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "name") : NULL;
        if (!cJSON_IsString(uid_j) || !uid_j->valuestring || uid_j->valuestring[0] == '\0') {
            ws_send_rsp(fd, id, false, "missing uid");
            return;
        }
        if (!cJSON_IsString(name_j) || !name_j->valuestring) {
            ws_send_rsp(fd, id, false, "missing name");
            return;
        }

        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_j->valuestring, sizeof(uid.uid));

        esp_err_t err = gw_device_registry_set_name(&uid, name_j->valuestring);
        if (err != ESP_OK) {
            ws_send_rsp(fd, id, false, (err == ESP_ERR_NOT_FOUND) ? "device not found" : "registry failed");
            return;
        }
        gw_event_bus_publish("device_renamed", "ws", uid.uid, 0, name_j->valuestring);
        ws_send_rsp(fd, id, true, NULL);
        return;
    }

    ws_send_rsp(fd, id, false, "unknown method");
}

static void ws_apply_subscriptions(int fd, cJSON *subs, uint32_t since)
{
    bool want_events = false;
    if (cJSON_IsArray(subs)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, subs)
        {
            if (cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, "events") == 0) {
                want_events = true;
            }
        }
    }

    portENTER_CRITICAL(&s_client_lock);
    for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].subscribed_events = want_events;
            break;
        }
    }
    portEXIT_CRITICAL(&s_client_lock);

    if (want_events) {
        ws_send_events_since(fd, since, 64);
    }
}

static void ws_handle_text(int fd, const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        (void)ws_send_json_async(fd, "{\"t\":\"rsp\",\"ok\":false,\"err\":\"invalid json\"}");
        return;
    }

    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(t) || !t->valuestring) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t->valuestring, "hello") == 0) {
        uint32_t since = 0;
        cJSON *since_j = cJSON_GetObjectItemCaseSensitive(root, "since");
        if (cJSON_IsNumber(since_j) && since_j->valuedouble >= 0) {
            since = (uint32_t)since_j->valuedouble;
        }
        cJSON *subs = cJSON_GetObjectItemCaseSensitive(root, "subs");
        ws_send_hello(fd);
        ws_apply_subscriptions(fd, subs, since);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t->valuestring, "sub") == 0) {
        cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
        uint32_t since = 0;
        cJSON *since_j = cJSON_GetObjectItemCaseSensitive(root, "since");
        if (cJSON_IsNumber(since_j) && since_j->valuedouble >= 0) {
            since = (uint32_t)since_j->valuedouble;
        }
        if (cJSON_IsString(topic) && topic->valuestring && strcmp(topic->valuestring, "events") == 0) {
            cJSON *subs = cJSON_CreateArray();
            cJSON_AddItemToArray(subs, cJSON_CreateString("events"));
            ws_apply_subscriptions(fd, subs, since);
            cJSON_Delete(subs);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t->valuestring, "unsub") == 0) {
        cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
        if (cJSON_IsString(topic) && topic->valuestring && strcmp(topic->valuestring, "events") == 0) {
            portENTER_CRITICAL(&s_client_lock);
            for (size_t i = 0; i < GW_WS_MAX_CLIENTS; i++) {
                if (s_clients[i].fd == fd) {
                    s_clients[i].subscribed_events = false;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_client_lock);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t->valuestring, "ping") == 0) {
        (void)ws_send_json_async(fd, "{\"t\":\"pong\"}");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t->valuestring, "req") == 0) {
        ws_handle_req(fd, root);
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        if (!ws_client_add_fd(fd)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too many ws clients");
            return ESP_FAIL;
        }
        ws_send_hello(fd);
        return ESP_OK;
    }

    // Reject non-WebSocket requests early to avoid confusing httpd_ws_recv_frame().
    if (!s_server || httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        ws_client_remove_fd(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv header failed: %s", esp_err_to_name(err));
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_client_remove_fd(fd);
        return ESP_OK;
    }

    if (frame.len > 4096) {
        ws_client_remove_fd(fd);
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        ws_handle_text(fd, (const char *)buf, frame.len);
    }

    free(buf);
    return ESP_OK;
}

esp_err_t gw_ws_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server) {
        return ESP_OK;
    }

    s_server = server;
    memset(s_clients, 0, sizeof(s_clients));

    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };

    esp_err_t err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    err = gw_event_bus_add_listener(ws_publish_event_to_clients, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event listener not installed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "WebSocket enabled at /ws");
    return ESP_OK;
}

void gw_ws_unregister(void)
{
    if (!s_server) {
        return;
    }
    (void)gw_event_bus_remove_listener(ws_publish_event_to_clients, NULL);

    portENTER_CRITICAL(&s_client_lock);
    memset(s_clients, 0, sizeof(s_clients));
    portEXIT_CRITICAL(&s_client_lock);
    s_server = NULL;
}
