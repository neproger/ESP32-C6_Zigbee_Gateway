#include "gw_http/gw_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "cJSON.h"

#include "gw_core/device_registry.h"
#include "gw_core/event_bus.h"
#include "gw_core/sensor_store.h"
#include "gw_core/zb_classify.h"
#include "gw_core/zb_model.h"
#include "gw_zigbee/gw_zigbee.h"
#include "gw_http/gw_ws.h"

static const char *TAG = "gw_http";

static httpd_handle_t s_server;
static uint16_t s_server_port;
static bool s_spiffs_mounted;

static const char *find_query_value(const char *query, const char *key, char *out, size_t out_size);

static esp_err_t api_events_get_handler(httpd_req_t *req);
static esp_err_t api_devices_remove_post_handler(httpd_req_t *req);
static esp_err_t api_endpoints_get_handler(httpd_req_t *req);
static esp_err_t api_sensors_get_handler(httpd_req_t *req);

static esp_err_t gw_http_spiffs_init(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted (%s): total=%u used=%u", conf.partition_label, (unsigned)total, (unsigned)used);
    }

    s_spiffs_mounted = true;
    return ESP_OK;
}

static const char *gw_http_content_type_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "text/plain";
    }
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".map") == 0) return "application/json";
    return "application/octet-stream";
}

static bool gw_http_uri_looks_like_asset(const char *uri)
{
    const char *slash = strrchr(uri, '/');
    const char *dot = strrchr(uri, '.');
    return (dot != NULL && (slash == NULL || dot > slash));
}

static esp_err_t gw_http_send_spiffs_file(httpd_req_t *req, const char *uri_path)
{
    if (!s_spiffs_mounted) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "web fs not mounted");
        return ESP_OK;
    }

    char fullpath[256];
    int n = snprintf(fullpath, sizeof(fullpath), "/www%s", uri_path);
    if (n <= 0 || n >= (int)sizeof(fullpath)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long");
        return ESP_OK;
    }

    FILE *f = fopen(fullpath, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, gw_http_content_type_from_path(fullpath));

    uint8_t buf[1024];
    while (true) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)r);
            if (err != ESP_OK) {
                fclose(f);
                httpd_resp_send_chunk(req, NULL, 0);
                return err;
            }
        }
        if (r < sizeof(buf)) {
            break;
        }
    }

    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    return gw_http_send_spiffs_file(req, "/index.html");
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    if (!s_spiffs_mounted) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "web fs not mounted");
        return ESP_OK;
    }

    const char *uri = req->uri;
    if (uri == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
        return ESP_OK;
    }

    // Prevent path traversal.
    if (strstr(uri, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    if (strcmp(uri, "/") == 0) {
        return gw_http_send_spiffs_file(req, "/index.html");
    }

    // Strip query (defensive; typically not present in req->uri).
    const char *q = strchr(uri, '?');
    size_t uri_len = (q != NULL) ? (size_t)(q - uri) : strlen(uri);
    if (uri_len == 0 || uri_len > 200) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "bad uri");
        return ESP_OK;
    }

    char path[256];
    int n = snprintf(path, sizeof(path), "%.*s", (int)uri_len, uri);
    if (n <= 0 || n >= (int)sizeof(path)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "bad uri");
        return ESP_OK;
    }

    // If file exists -> serve it.
    char fullpath[256];
    n = snprintf(fullpath, sizeof(fullpath), "/www%s", path);
    if (n > 0 && n < (int)sizeof(fullpath)) {
        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
            return gw_http_send_spiffs_file(req, path);
        }
    }

    // SPA fallback: if it's not an asset path, serve index.html for client-side routing.
    if (!gw_http_uri_looks_like_asset(path)) {
        return gw_http_send_spiffs_file(req, "/index.html");
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    const size_t max_devices = 32;
    gw_device_t *devices = (gw_device_t *)calloc(max_devices, sizeof(gw_device_t));
    if (devices == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    size_t count = gw_device_registry_list(devices, max_devices);

    esp_err_t err = httpd_resp_sendstr_chunk(req, "[");
    if (err != ESP_OK) {
        free(devices);
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        const gw_device_t *d = &devices[i];
        char line[192];
        int n = snprintf(line,
                         sizeof(line),
                         "%s{\"device_uid\":\"%s\",\"name\":\"%s\",\"short_addr\":%u,\"has_onoff\":%s,\"has_button\":%s}",
                         (i == 0 ? "" : ","),
                         d->device_uid.uid,
                         d->name,
                         (unsigned)d->short_addr,
                         d->has_onoff ? "true" : "false",
                         d->has_button ? "true" : "false");
        if (n < 0) {
            free(devices);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "format error");
            return ESP_OK;
        }

        err = httpd_resp_sendstr_chunk(req, line);
        if (err != ESP_OK) {
            free(devices);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }
    }

    free(devices);

    err = httpd_resp_sendstr_chunk(req, "]");
    if (err != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_endpoints_get_handler(httpd_req_t *req)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    char uid_s[GW_DEVICE_UID_STRLEN] = {0};
    if (find_query_value(query, "uid", uid_s, sizeof(uid_s)) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uid");
        return ESP_OK;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_s, sizeof(uid.uid));

    const size_t max_eps = 16;
    gw_zb_endpoint_t *eps = (gw_zb_endpoint_t *)calloc(max_eps, sizeof(gw_zb_endpoint_t));
    if (eps == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    size_t count = gw_zb_model_list_endpoints(&uid, eps, max_eps);

    // Avoid large stack frames in the httpd task.
    char *accepts = (char *)malloc(1024);
    char *emits = (char *)malloc(1024);
    char *reports = (char *)malloc(1024);
    if (!accepts || !emits || !reports) {
        free(accepts);
        free(emits);
        free(reports);
        free(eps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        free(accepts);
        free(emits);
        free(reports);
        free(eps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    for (size_t i = 0; i < count; i++) {
        const gw_zb_endpoint_t *e = &eps[i];

        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            free(accepts);
            free(emits);
            free(reports);
            free(eps);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
            return ESP_OK;
        }

        cJSON_AddNumberToObject(o, "endpoint", (double)e->endpoint);
        cJSON_AddNumberToObject(o, "profile_id", (double)e->profile_id);
        cJSON_AddNumberToObject(o, "device_id", (double)e->device_id);

        cJSON *in = cJSON_AddArrayToObject(o, "in_clusters");
        for (uint8_t c = 0; c < e->in_cluster_count; c++) {
            cJSON_AddItemToArray(in, cJSON_CreateNumber((double)e->in_clusters[c]));
        }
        cJSON *out = cJSON_AddArrayToObject(o, "out_clusters");
        for (uint8_t c = 0; c < e->out_cluster_count; c++) {
            cJSON_AddItemToArray(out, cJSON_CreateNumber((double)e->out_clusters[c]));
        }

        cJSON_AddStringToObject(o, "kind", gw_zb_endpoint_kind(e));
        {
            gw_zb_endpoint_accepts_json(e, accepts, 1024);
            gw_zb_endpoint_emits_json(e, emits, 1024);
            gw_zb_endpoint_reports_json(e, reports, 1024);

            cJSON *a = cJSON_Parse(accepts);
            cJSON *m = cJSON_Parse(emits);
            cJSON *r = cJSON_Parse(reports);

            if (!a) a = cJSON_CreateArray();
            if (!m) m = cJSON_CreateArray();
            if (!r) r = cJSON_CreateArray();

            if (a) cJSON_AddItemToObject(o, "accepts", a);
            if (m) cJSON_AddItemToObject(o, "emits", m);
            if (r) cJSON_AddItemToObject(o, "reports", r);
        }

        cJSON_AddItemToArray(arr, o);
    }

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    free(accepts);
    free(emits);
    free(reports);
    free(eps);
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    cJSON_free(s);
    return err;
}

static esp_err_t api_sensors_get_handler(httpd_req_t *req)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    char uid_s[GW_DEVICE_UID_STRLEN] = {0};
    if (find_query_value(query, "uid", uid_s, sizeof(uid_s)) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uid");
        return ESP_OK;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_s, sizeof(uid.uid));

    const size_t max_vals = 32;
    gw_sensor_value_t *vals = (gw_sensor_value_t *)calloc(max_vals, sizeof(gw_sensor_value_t));
    if (vals == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    size_t count = gw_sensor_store_list(&uid, vals, max_vals);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr_chunk(req, "[");
    if (err != ESP_OK) {
        free(vals);
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        const gw_sensor_value_t *v = &vals[i];
        char line[196];
        const char *key = (v->value_type == GW_SENSOR_VALUE_I32) ? "value_i32" : "value_u32";
        long long ts = (long long)v->ts_ms;
        long long val = (v->value_type == GW_SENSOR_VALUE_I32) ? (long long)v->value_i32 : (long long)v->value_u32;

        int n = snprintf(line,
                         sizeof(line),
                         "%s{\"endpoint\":%u,\"cluster_id\":%u,\"attr_id\":%u,\"%s\":%lld,\"ts_ms\":%lld}",
                         (i == 0 ? "" : ","),
                         (unsigned)v->endpoint,
                         (unsigned)v->cluster_id,
                         (unsigned)v->attr_id,
                         key,
                         val,
                         ts);
        if (n < 0) {
            free(vals);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "format error");
            return ESP_OK;
        }
        err = httpd_resp_sendstr_chunk(req, line);
        if (err != ESP_OK) {
            free(vals);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }
    }

    free(vals);
    err = httpd_resp_sendstr_chunk(req, "]");
    if (err != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static const char *find_query_value(const char *query, const char *key, char *out, size_t out_size)
{
    if (query == NULL || key == NULL || out == NULL || out_size == 0) {
        return NULL;
    }
    // naive query parsing: key=value&...
    const char *p = query;
    size_t key_len = strlen(key);
    while (*p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_size) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return out;
        }
        while (*p && *p != '&') {
            p++;
        }
        if (*p == '&') {
            p++;
        }
    }
    return NULL;
}

static esp_err_t gw_http_json_send_escaped(httpd_req_t *req, const char *s)
{
    if (s == NULL) {
        s = "";
    }

    char out[96];
    size_t out_len = 0;

    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        const unsigned char c = *p++;
        const char *rep = NULL;
        char esc[8];

        switch (c) {
        case '\\':
            rep = "\\\\";
            break;
        case '"':
            rep = "\\\"";
            break;
        case '\n':
            rep = "\\n";
            break;
        case '\r':
            rep = "\\r";
            break;
        case '\t':
            rep = "\\t";
            break;
        default:
            if (c < 0x20) {
                (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                rep = esc;
            }
            break;
        }

        if (rep != NULL) {
            size_t rep_len = strlen(rep);
            if (out_len + rep_len >= sizeof(out)) {
                out[out_len] = '\0';
                esp_err_t err = httpd_resp_sendstr_chunk(req, out);
                if (err != ESP_OK) {
                    return err;
                }
                out_len = 0;
            }
            memcpy(&out[out_len], rep, rep_len);
            out_len += rep_len;
            continue;
        }

        if (out_len + 1 >= sizeof(out)) {
            out[out_len] = '\0';
            esp_err_t err = httpd_resp_sendstr_chunk(req, out);
            if (err != ESP_OK) {
                return err;
            }
            out_len = 0;
        }
        out[out_len++] = (char)c;
    }

    if (out_len > 0) {
        out[out_len] = '\0';
        return httpd_resp_sendstr_chunk(req, out);
    }

    return ESP_OK;
}

static esp_err_t api_devices_post_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    char uid[GW_DEVICE_UID_STRLEN] = {0};
    char name[sizeof(((gw_device_t *)0)->name)] = {0};
    char onoff[8] = {0};
    char button[8] = {0};

    if (find_query_value(query, "uid", uid, sizeof(uid)) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uid");
        return ESP_OK;
    }
    const bool has_name = (find_query_value(query, "name", name, sizeof(name)) != NULL);
    const bool has_onoff = (find_query_value(query, "onoff", onoff, sizeof(onoff)) != NULL);
    const bool has_button = (find_query_value(query, "button", button, sizeof(button)) != NULL);

    gw_device_t d = {0};
    strlcpy(d.device_uid.uid, uid, sizeof(d.device_uid.uid));
    if (gw_device_registry_get(&d.device_uid, &d) != ESP_OK) {
        // New record: keep defaults (short_addr=0, caps=false, name empty) unless provided below.
        memset(&d, 0, sizeof(d));
        strlcpy(d.device_uid.uid, uid, sizeof(d.device_uid.uid));
    }

    if (has_name) {
        // Allow empty string to clear name.
        strlcpy(d.name, name, sizeof(d.name));
    }
    if (has_onoff) {
        d.has_onoff = (onoff[0] == '1');
    }
    if (has_button) {
        d.has_button = (button[0] == '1');
    }

    esp_err_t err = gw_device_registry_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "registry error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_devices_remove_post_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    char uid_s[GW_DEVICE_UID_STRLEN] = {0};
    if (find_query_value(query, "uid", uid_s, sizeof(uid_s)) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uid");
        return ESP_OK;
    }

    bool kick = false;
    char kick_s[8] = {0};
    if (find_query_value(query, "kick", kick_s, sizeof(kick_s)) != NULL) {
        kick = (kick_s[0] == '1' || kick_s[0] == 't' || kick_s[0] == 'T' || kick_s[0] == 'y' || kick_s[0] == 'Y');
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, uid_s, sizeof(uid.uid));

    uint16_t short_addr = 0;
    if (kick) {
        gw_device_t d = {0};
        esp_err_t err = gw_device_registry_get(&uid, &d);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not found");
            return ESP_OK;
        }
        short_addr = d.short_addr;

        err = gw_zigbee_device_leave(&uid, short_addr, false);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "leave failed");
            return ESP_OK;
        }

        char msg[64];
        (void)snprintf(msg, sizeof(msg), "uid=%s short=0x%04x", uid.uid, (unsigned)short_addr);
        gw_event_bus_publish("api_device_kick", "http", uid.uid, short_addr, msg);
    }

    esp_err_t rm = gw_device_registry_remove(&uid);
    if (rm != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not found");
        return ESP_OK;
    }

    gw_event_bus_publish("api_device_removed", "http", uid.uid, short_addr, kick ? "kick=1" : "kick=0");

    httpd_resp_set_type(req, "application/json");
    char resp[96];
    int n = snprintf(resp,
                     sizeof(resp),
                     "{\"ok\":true,\"uid\":\"%s\",\"kick\":%s}",
                     uid.uid,
                     kick ? "true" : "false");
    return httpd_resp_send(req, resp, n);
}

static esp_err_t api_network_permit_join_post_handler(httpd_req_t *req)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    uint8_t seconds = 180;
    char seconds_s[16] = {0};
    if (find_query_value(query, "seconds", seconds_s, sizeof(seconds_s)) != NULL) {
        long v = strtol(seconds_s, NULL, 10);
        if (v > 0 && v <= 255) {
            seconds = (uint8_t)v;
        }
    }

    esp_err_t err = gw_zigbee_permit_join(seconds);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "permit_join failed");
        return ESP_OK;
    }

    {
        char msg[48];
        (void)snprintf(msg, sizeof(msg), "seconds=%u", (unsigned)seconds);
        gw_event_bus_publish("api_permit_join", "http", "", 0, msg);
    }

    char resp[64];
    int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"seconds\":%u}", (unsigned)seconds);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

static esp_err_t api_events_get_handler(httpd_req_t *req)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    uint32_t since = 0;
    char since_s[16] = {0};
    if (find_query_value(query, "since", since_s, sizeof(since_s)) != NULL) {
        unsigned long v = strtoul(since_s, NULL, 10);
        since = (uint32_t)v;
    }

    size_t limit = 64;
    char limit_s[16] = {0};
    if (find_query_value(query, "limit", limit_s, sizeof(limit_s)) != NULL) {
        unsigned long v = strtoul(limit_s, NULL, 10);
        if (v >= 1 && v <= 128) {
            limit = (size_t)v;
        }
    }

    gw_event_t *events = (gw_event_t *)calloc(limit, sizeof(gw_event_t));
    if (events == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    uint32_t last_id = 0;
    size_t count = gw_event_bus_list_since(since, events, limit, &last_id);

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr_chunk(req, "{\"last_id\":");
    if (err != ESP_OK) {
        free(events);
        return err;
    }

    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%u", (unsigned)last_id);
    if (n < 0) {
        free(events);
        httpd_resp_sendstr_chunk(req, NULL);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "format error");
        return ESP_OK;
    }

    err = httpd_resp_sendstr_chunk(req, tmp);
    if (err != ESP_OK) {
        free(events);
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, ",\"events\":[");
    if (err != ESP_OK) {
        free(events);
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        const gw_event_t *e = &events[i];
        err = httpd_resp_sendstr_chunk(req, (i == 0) ? "{" : ",{");
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }

        n = snprintf(tmp, sizeof(tmp), "\"id\":%u,\"ts_ms\":%llu,", (unsigned)e->id, (unsigned long long)e->ts_ms);
        if (n < 0) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "format error");
            return ESP_OK;
        }
        err = httpd_resp_sendstr_chunk(req, tmp);
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }

        err = httpd_resp_sendstr_chunk(req, "\"type\":\"");
        if (err == ESP_OK) err = gw_http_json_send_escaped(req, e->type);
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\",\"source\":\"");
        if (err == ESP_OK) err = gw_http_json_send_escaped(req, e->source);
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\",\"device_uid\":\"");
        if (err == ESP_OK) err = gw_http_json_send_escaped(req, e->device_uid);
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\"");
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }

        n = snprintf(tmp, sizeof(tmp), ",\"short_addr\":%u,\"msg\":\"", (unsigned)e->short_addr);
        if (n < 0) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "format error");
            return ESP_OK;
        }
        err = httpd_resp_sendstr_chunk(req, tmp);
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }

        err = gw_http_json_send_escaped(req, e->msg);
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }

        err = httpd_resp_sendstr_chunk(req, "\"}");
        if (err != ESP_OK) {
            free(events);
            httpd_resp_sendstr_chunk(req, NULL);
            return err;
        }
    }

    free(events);

    err = httpd_resp_sendstr_chunk(req, "]}");
    if (err != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

esp_err_t gw_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    (void)gw_http_spiffs_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // We register several API endpoints + a wildcard handler for SPA/static files.
    // Default (8) is too small once UI grows.
    config.max_uri_handlers = 16;
    s_server_port = config.server_port;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server_port = 0;
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_devices_get_uri = {
        .uri = "/api/devices",
        .method = HTTP_GET,
        .handler = api_devices_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_devices_post_uri = {
        .uri = "/api/devices",
        .method = HTTP_POST,
        .handler = api_devices_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_endpoints_get_uri = {
        .uri = "/api/endpoints",
        .method = HTTP_GET,
        .handler = api_endpoints_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_sensors_get_uri = {
        .uri = "/api/sensors",
        .method = HTTP_GET,
        .handler = api_sensors_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_devices_remove_post_uri = {
        .uri = "/api/devices/remove",
        .method = HTTP_POST,
        .handler = api_devices_remove_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_network_permit_join_post_uri = {
        .uri = "/api/network/permit_join",
        .method = HTTP_POST,
        .handler = api_network_permit_join_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_events_get_uri = {
        .uri = "/api/events",
        .method = HTTP_GET,
        .handler = api_events_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_get_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_devices_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_devices_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_endpoints_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_sensors_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_devices_remove_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_network_permit_join_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_events_get_uri));
    ESP_ERROR_CHECK(gw_ws_register(s_server));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &static_uri));

    if (s_server_port != 0) {
        ESP_LOGI(TAG, "HTTP server started (port %u)", (unsigned)s_server_port);
    } else {
        ESP_LOGI(TAG, "HTTP server started");
    }
    return ESP_OK;
}

esp_err_t gw_http_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    gw_ws_unregister();
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    s_server_port = 0;
    return err;
}

uint16_t gw_http_get_port(void)
{
    return s_server_port;
}
