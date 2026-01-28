#include "gw_http/gw_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "gw_core/device_registry.h"
#include "gw_zigbee/gw_zigbee.h"

static const char *TAG = "gw_http";

static httpd_handle_t s_server;
static uint16_t s_server_port;
static bool s_spiffs_mounted;

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

static esp_err_t api_devices_post_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }

    gw_device_t d = {0};
    char uid[GW_DEVICE_UID_STRLEN] = {0};
    char name[sizeof(d.name)] = {0};
    char onoff[8] = {0};
    char button[8] = {0};

    if (find_query_value(query, "uid", uid, sizeof(uid)) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing uid");
        return ESP_OK;
    }
    find_query_value(query, "name", name, sizeof(name));
    find_query_value(query, "onoff", onoff, sizeof(onoff));
    find_query_value(query, "button", button, sizeof(button));

    strlcpy(d.device_uid.uid, uid, sizeof(d.device_uid.uid));
    strlcpy(d.name, name[0] ? name : "Device", sizeof(d.name));
    d.short_addr = 0;
    d.has_onoff = (onoff[0] == '1');
    d.has_button = (button[0] == '1');

    esp_err_t err = gw_device_registry_upsert(&d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registry upsert failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "registry error");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
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

    char resp[64];
    int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"seconds\":%u}", (unsigned)seconds);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

esp_err_t gw_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    (void)gw_http_spiffs_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
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
    static const httpd_uri_t api_network_permit_join_post_uri = {
        .uri = "/api/network/permit_join",
        .method = HTTP_POST,
        .handler = api_network_permit_join_post_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_network_permit_join_post_uri));
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

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    s_server_port = 0;
    return err;
}

uint16_t gw_http_get_port(void)
{
    return s_server_port;
}

