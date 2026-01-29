#include "gw_core/event_bus.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

ESP_EVENT_DEFINE_BASE(GW_EVENT_BASE);

static const char *TAG = "gw_event";

static bool s_inited;

// In-memory ring buffer for UI/debugging. Keep small and fixed-size.
#define GW_EVENT_RING_CAP 64

static gw_event_t s_ring[GW_EVENT_RING_CAP];
static size_t s_ring_head;   // next write
static size_t s_ring_count;  // <= GW_EVENT_RING_CAP
static uint32_t s_next_id = 1;
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

// Optional listeners called on publish.
#define GW_EVENT_LISTENER_CAP 4
typedef struct {
    gw_event_bus_listener_t cb;
    void *user_ctx;
} gw_event_listener_slot_t;
static gw_event_listener_slot_t s_listeners[GW_EVENT_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static void safe_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const char *payload_json);

esp_err_t gw_event_bus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    // Uses the default event loop created by esp_event_loop_create_default()
    portENTER_CRITICAL(&s_ring_lock);
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head = 0;
    s_ring_count = 0;
    s_next_id = 1;
    portEXIT_CRITICAL(&s_ring_lock);

    portENTER_CRITICAL(&s_listener_lock);
    memset(s_listeners, 0, sizeof(s_listeners));
    portEXIT_CRITICAL(&s_listener_lock);

    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_post(GW_EVENT_BASE, (int32_t)id, data, data_size, ticks_to_wait);
}

uint32_t gw_event_bus_last_id(void)
{
    uint32_t last = 0;
    portENTER_CRITICAL(&s_ring_lock);
    if (s_ring_count > 0) {
        size_t newest = (s_ring_head + GW_EVENT_RING_CAP - 1) % GW_EVENT_RING_CAP;
        last = s_ring[newest].id;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    return last;
}

void gw_event_bus_publish(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *msg)
{
    gw_event_bus_publish_ex(type, source, device_uid, short_addr, msg, NULL);
}

void gw_event_bus_publish_json(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *payload_json)
{
    // Legacy helper: publish a structured payload without forcing callers to build a human msg.
    // msg is left empty to avoid re-introducing the old "JSON-in-msg" pattern.
    gw_event_bus_publish_ex(type, source, device_uid, short_addr, "", payload_json);
}

void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const char *payload_json)
{
    if (!s_inited) {
        return;
    }

    gw_event_t e = {0};
    e.v = 1;
    e.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    safe_copy_str(e.type, sizeof(e.type), type);
    safe_copy_str(e.source, sizeof(e.source), source);
    safe_copy_str(e.device_uid, sizeof(e.device_uid), device_uid);
    e.short_addr = short_addr;
    safe_copy_str(e.msg, sizeof(e.msg), msg);
    safe_copy_str(e.payload_json, sizeof(e.payload_json), payload_json);

    portENTER_CRITICAL(&s_ring_lock);
    e.id = s_next_id++;
    s_ring[s_ring_head] = e;
    s_ring_head = (s_ring_head + 1) % GW_EVENT_RING_CAP;
    if (s_ring_count < GW_EVENT_RING_CAP) {
        s_ring_count++;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    // Notify listeners outside of the ring critical section.
    gw_event_listener_slot_t listeners[GW_EVENT_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(&e, listeners[i].user_ctx);
    }

    // Duplicate event log to UART/monitor for convenience.
    ESP_LOGI(TAG,
             "#%u %s/%s uid=%s short=0x%04x %s",
             (unsigned)e.id,
             e.source,
             e.type,
             e.device_uid[0] ? e.device_uid : "-",
             (unsigned)e.short_addr,
             e.msg[0] ? e.msg : "-");
}

size_t gw_event_bus_list_since(uint32_t since_id, gw_event_t *out, size_t max_out, uint32_t *out_last_id)
{
    if (!s_inited || out == NULL || max_out == 0) {
        if (out_last_id) {
            *out_last_id = 0;
        }
        return 0;
    }

    size_t written = 0;
    portENTER_CRITICAL(&s_ring_lock);

    uint32_t last = 0;
    if (s_ring_count > 0) {
        size_t newest = (s_ring_head + GW_EVENT_RING_CAP - 1) % GW_EVENT_RING_CAP;
        last = s_ring[newest].id;
    }

    size_t start = (s_ring_head + GW_EVENT_RING_CAP - s_ring_count) % GW_EVENT_RING_CAP;
    for (size_t i = 0; i < s_ring_count && written < max_out; i++) {
        const gw_event_t *e = &s_ring[(start + i) % GW_EVENT_RING_CAP];
        if (e->id > since_id) {
            out[written++] = *e;
        }
    }

    portEXIT_CRITICAL(&s_ring_lock);

    if (out_last_id) {
        *out_last_id = last;
    }
    return written;
}

esp_err_t gw_event_bus_add_listener(gw_event_bus_listener_t cb, void *user_ctx)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == NULL) {
            s_listeners[i].cb = cb;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_event_bus_remove_listener(gw_event_bus_listener_t cb, void *user_ctx)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_EVENT_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

