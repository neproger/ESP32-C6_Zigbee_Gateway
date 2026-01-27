#include "gw_http/gw_http.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "gw_core/device_registry.h"

static const char *TAG = "gw_http";

static httpd_handle_t s_server;

static const char *INDEX_HTML =
    "<!doctype html>\n"
    "<html lang=\"ru\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\"/>\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/>\n"
    "  <title>Zigbee Gateway</title>\n"
    "  <style>\n"
    "    body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:16px;max-width:980px}\n"
    "    h1{font-size:20px;margin:0 0 12px}\n"
    "    .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}\n"
    "    button{padding:8px 12px;cursor:pointer}\n"
    "    table{width:100%;border-collapse:collapse;margin-top:12px}\n"
    "    th,td{border-bottom:1px solid #ddd;padding:8px;text-align:left;font-size:14px}\n"
    "    code{background:#f6f6f6;padding:2px 4px;border-radius:4px}\n"
    "    .muted{color:#666;font-size:13px}\n"
    "    form{margin-top:16px;padding:12px;border:1px solid #ddd;border-radius:8px}\n"
    "    input{padding:8px;min-width:220px}\n"
    "    label{display:block;margin:8px 0 4px}\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>ESP32‑C6 Zigbee Gateway</h1>\n"
    "  <div class=\"row\">\n"
    "    <button onclick=\"loadDevices()\">Обновить список</button>\n"
    "    <span class=\"muted\">Устройства берутся из реестра прошивки (пока без Zigbee‑интервью).</span>\n"
    "  </div>\n"
    "  <table>\n"
    "    <thead><tr><th>UID</th><th>Имя</th><th>Short</th><th>Caps</th></tr></thead>\n"
    "    <tbody id=\"tbody\"></tbody>\n"
    "  </table>\n"
    "\n"
    "  <form onsubmit=\"return addDevice(event)\">\n"
    "    <div class=\"muted\">Временная форма для теста UI (ручное добавление в реестр).</div>\n"
    "    <label>device_uid (пример: <code>0x00124B0012345678</code>)</label>\n"
    "    <input id=\"uid\" value=\"0x00124B0012345678\"/>\n"
    "    <label>name</label>\n"
    "    <input id=\"name\" value=\"Device\"/>\n"
    "    <div class=\"row\" style=\"margin-top:10px\">\n"
    "      <label style=\"margin:0\"><input type=\"checkbox\" id=\"cap_onoff\" checked/> onoff</label>\n"
    "      <label style=\"margin:0\"><input type=\"checkbox\" id=\"cap_button\"/> button</label>\n"
    "    </div>\n"
    "    <div class=\"row\" style=\"margin-top:10px\">\n"
    "      <button type=\"submit\">Добавить/обновить</button>\n"
    "    </div>\n"
    "  </form>\n"
    "\n"
    "  <script>\n"
    "    async function loadDevices(){\n"
    "      const r = await fetch('/api/devices');\n"
    "      const items = await r.json();\n"
    "      const tbody = document.getElementById('tbody');\n"
    "      tbody.innerHTML='';\n"
    "      for(const d of items){\n"
    "        const caps=[];\n"
    "        if(d.has_onoff) caps.push('onoff');\n"
    "        if(d.has_button) caps.push('button');\n"
    "        const tr=document.createElement('tr');\n"
    "        tr.innerHTML = `<td><code>${d.device_uid}</code></td><td>${d.name||''}</td><td><code>0x${(d.short_addr||0).toString(16)}</code></td><td>${caps.join(', ')}</td>`;\n"
    "        tbody.appendChild(tr);\n"
    "      }\n"
    "    }\n"
    "    async function addDevice(e){\n"
    "      e.preventDefault();\n"
    "      const uid=document.getElementById('uid').value;\n"
    "      const name=document.getElementById('name').value;\n"
    "      const onoff=document.getElementById('cap_onoff').checked ? '1' : '0';\n"
    "      const button=document.getElementById('cap_button').checked ? '1' : '0';\n"
    "      const q=new URLSearchParams({uid,name,onoff,button});\n"
    "      await fetch('/api/devices?'+q.toString(), {method:'POST'});\n"
    "      await loadDevices();\n"
    "      return false;\n"
    "    }\n"
    "    loadDevices();\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    (void)req;

    gw_device_t devices[32];
    size_t count = gw_device_registry_list(devices, sizeof(devices) / sizeof(devices[0]));

    // Minimal JSON serialization to avoid bringing full JSON libs right now.
    // Output example: [{"device_uid":"0x..","name":"..","short_addr":4660,"has_onoff":true,"has_button":false},...]
    char buf[4096];
    size_t off = 0;
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "[");
    for (size_t i = 0; i < count && off < sizeof(buf); i++) {
        const gw_device_t *d = &devices[i];
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%s{\"device_uid\":\"%s\",\"name\":\"%s\",\"short_addr\":%u,\"has_onoff\":%s,\"has_button\":%s}",
                                (i == 0 ? "" : ","),
                                d->device_uid.uid,
                                d->name,
                                (unsigned)d->short_addr,
                                d->has_onoff ? "true" : "false",
                                d->has_button ? "true" : "false");
    }
    if (off < sizeof(buf)) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "]");
    } else {
        // Ensure valid JSON even if truncated
        buf[sizeof(buf) - 2] = ']';
        buf[sizeof(buf) - 1] = '\0';
        off = strlen(buf);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (ssize_t)off);
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

esp_err_t gw_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
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

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_devices_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &api_devices_post_uri));

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t gw_http_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

