# Event Pipeline Tuning Guide

## Overview
The ESP32-C6 Zigbee Gateway uses an event-driven architecture with multiple async pipelines (rules engine, WebSocket broadcast, etc.). This document provides guidelines for tuning queue sizes and task priorities to match your deployment.

## Event Flow Architecture

```
Zigbee callbacks (on core X)
  └─> gw_event_bus_publish_ex()
      ├─> Sync listener: rules_event_listener()
      │   └─> enqueue event into rules queue
      │       └─> rules_task processes (FreeRTOS task, priority 5)
      │           ├─> compile triggers/conditions
      │           └─> execute actions
      │
      └─> Sync listener: ws_publish_event_to_clients()
          └─> enqueue event into ws queue
              └─> ws_event_task processes (FreeRTOS task, priority 4)
                  ├─> build JSON with cJSON
                  └─> send to all subscribed clients (async http)
```

## Queue Tuning Parameters

### 1. **Event Ring Buffer** (`gw_event_bus.c`)
- **Current:** 64 events
- **Purpose:** In-memory circular buffer for UI polling (REST API `GET /api/events`)
- **Impact:** Low — mostly informational; overwrite just means old events lost
- **Recommendation:** Keep at 64 (minimal memory footprint)
- **When to increase:** If you need longer historical event log for debugging

```c
#define GW_EVENT_RING_CAP 64
```

### 2. **Rules Engine Queue** (`rules_engine.c`)
- **Current:** 16 events
- **Purpose:** Buffer events between publisher and rules processing
- **Impact:** High — if queue overflows, automations miss triggers
- **Recommendation:** **Increase to 32–64** depending on event burstiness
  - 32: normal deployments (few rules, moderate button presses)
  - 64: high-frequency sensors (many zigbee.attr_report events per second)
- **Monitor:** Watch for `"rules event queue overflow"` in logs
  - Indicates slow rules processing or too many simultaneous events
  - Consider increasing task stack or moving to parallel execution

```c
// In gw_rules_init():
s_q = xQueueCreate(32, sizeof(gw_event_t));  // Increase from 16
```

### 3. **WebSocket Event Queue** (`gw_ws.c`)
- **Current:** 32 events
- **Purpose:** Buffer events between publisher and WS JSON building
- **Impact:** Medium — affects UI responsiveness; overflows cause clients to miss events
- **Recommendation:** Keep at 32–48
  - 32: small deployments (< 4 simultaneous browser tabs)
  - 48: busy UIs with many open connections

```c
// In gw_ws_register():
s_event_q = xQueueCreate(32, sizeof(gw_event_t));  // OK for most deployments
```

### 4. **State Store** (`state_store.c`)
- **Current:** ~32 items (fixed LRU, evicts oldest)
- **Purpose:** Cache of device states for condition evaluation
- **Impact:** Low — if evicted, condition evaluation fails; rules simply don't fire
- **Recommendation:** **Increase to 64–128** if you have > 16 devices
  - Each device × ~3 keys (temperature_c, humidity_pct, battery_pct) = ~48 items for 16 devices
  - Current default of 32 may evict state while rules are still evaluating

```c
#define GW_STATE_MAX_ITEMS 32  // In state_store.c — increase to 64
```

## Task Priorities & Stack Sizes

| Task | Priority | Stack | Notes |
|------|----------|-------|-------|
| rules_task | 5 | 4096 | Processes event triggers/conditions/actions |
| ws_event_task | 4 | 4096 | JSON serialization + async send |
| esp_zb_task (Zigbee) | 5 | 8192 | Radio RX/TX, command parsing |
| http_server (esp_http_server) | 20 (low) | varies | Handles REST/WS connections |

**Guidance:**
- Do **not** increase rule task priority above Zigbee task (5) — risk of starvation
- WS task priority 4 is fine (lower than rules = rules have priority)
- If rules are slow, increase stack (4096 → 6144) or profile the compilation step

## Detecting Bottlenecks

### 1. Queue Overflow (New)
Monitor for these log messages:
```
W (12345) gw_rules: rules event queue overflow (id=100 type=zigbee.command); event lost
W (12350) gw_ws: ws event queue overflow (id=101 type=rules.fired); ws clients may miss event
```
**Action:** Increase corresponding queue size (see above).

### 2. State Store Eviction
Add logging to `state_store.c` if you suspect eviction:
```c
// In upsert_item() when evicting oldest:
ESP_LOGW("gw_state", "state eviction: dropping oldest (uid=%s key=%s)", 
         s_items[idx].uid.uid, s_items[idx].key);
```
**Action:** Increase `GW_STATE_MAX_ITEMS` to 64.

### 3. Zigbee Command Latency
Compare timestamps in logs:
```
I (1509404) gw_event: #466 zigbee/zigbee.cmd_sent ...
I (1510374) gw_event: #469 zigbee/zigbee.command (toggle)  <-- 970ms later!
```
If large gaps appear between command and action, check:
1. Zigbee radio is not congested (RF interference, too many devices)
2. ESP32-C6 CPU is not overloaded (check free heap, task monitor)
3. Event pipeline queues are not overflowing

### 4. Event Ordering Anomalies
If `rules.fired` appears *before* the command event that triggered it, there's a race condition or extreme queue backpressure. Example (BAD):
```
I (1510370) gw_event: #468 rules/rules.fired ...   <-- fired first
I (1510374) gw_event: #469 zigbee/zigbee.command   <-- command arrived later
```
**Solution:** Check if rules queue is getting full; increase size.

## Example: High-Load Scenario

If you have:
- 20+ Zigbee devices (each sending temp/humidity every ~10 seconds)
- 5+ automations (each with 2–3 conditions)
- 2–3 active browser tabs (WebSocket)

**Recommended settings:**
```c
// gw_rules_init():
s_q = xQueueCreate(64, sizeof(gw_event_t));  // Handle 64 events

// state_store.c:
#define GW_STATE_MAX_ITEMS 128  // ~20 devices × 3–4 keys each

// gw_ws_register():
s_event_q = xQueueCreate(48, sizeof(gw_event_t));  // UI stays responsive

// gw_event_bus.c (optional):
#define GW_EVENT_RING_CAP 128  // Keep longer history
```

## Performance Metrics to Log

Consider adding a periodic task to log:
```c
uint32_t rules_queue_depth = uxQueueSpacesAvailable(s_q);
uint32_t ws_queue_depth = uxQueueSpacesAvailable(s_event_q);
ESP_LOGI(TAG, "queue_depth: rules=%u ws=%u heap=%u", 
         16 - rules_queue_depth, 32 - ws_queue_depth, 
         esp_get_free_heap_size());
```
This helps identify trends and plan upgrades.

## References
- [FreeRTOS Queue API](https://www.freertos.org/a00019.html)
- [ESP-IDF Event Loop](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/esp_event.html)
- [Zigbee Stack Guide](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/overview.html)
