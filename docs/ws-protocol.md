# WebSocket protocol (`/ws`)

The gateway exposes a JSON-over-WebSocket API at `ws://<gw-ip>/ws` (or `wss://` if served over HTTPS).

## Version

- `proto`: `gw-ws-1`

## Updating the web UI on device

From `web/` you can rebuild the UI and flash only the `www` SPIFFS partition (no firmware flash) using:

```sh
npm run esp -- -p COM5
```

## Messages (client → server)

### `hello`

Subscribe and optionally request replay of missed events.

```json
{ "t": "hello", "proto": "gw-ws-1", "subs": ["events"], "since": 123 }
```

- `subs`: currently supported: `["events"]`
- `since`: last event id you have; server will replay `id > since` (up to the in-memory ring capacity)

### `req`

Request/response RPC. Client chooses `id` (string/number) for correlation.

```json
{ "t": "req", "id": 1, "m": "events.list", "p": { "since": 0, "limit": 64 } }
```

Supported methods:

- `events.list` → `{ last_id, events: [...] }`
- `automations.list` → `{ automations: [...] }`
- `automations.put` (`id`, `name`, optional `enabled`, `json` string)
- `automations.remove` (`id`)
- `automations.set_enabled` (`id`, `enabled` boolean)
- `network.permit_join` (`seconds` 1..255)
- `devices.remove` (`uid` string, optional `kick` boolean)
- `devices.set_name` (`uid` string, `name` string)

### `ping`

```json
{ "t": "ping" }
```

## Messages (server → client)

### `hello`

```json
{ "t": "hello", "proto": "gw-ws-1", "caps": { "events": true, "req": true }, "event_last_id": 456 }
```

### `event`

Streamed when subscribed to `events`:

```json
{
  "t": "event",
  "id": 457,
  "ts_ms": 123456,
  "type": "zigbee_join",
  "source": "zb",
  "device_uid": "00124b...",
  "short_addr": 581,
  "msg": "..."
}
```

### `rsp`

Response to `req`:

```json
{ "t": "rsp", "id": 1, "ok": true, "res": { "last_id": 456, "events": [] } }
```

or

```json
{ "t": "rsp", "id": 1, "ok": false, "err": "..." }
```
