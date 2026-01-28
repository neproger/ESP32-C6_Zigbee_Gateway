# Roadmap / Checklist

Этот файл — “что делать дальше” в виде последовательного чеклиста.

## MVP-0 (стабилизация текущего состояния)

- [ ] `Wi‑Fi`: добавить fallback для скрытых SSID (если scan не видит, всё равно пробовать подключиться по списку).
- [ ] `HTTP`: добавить `/health` (например `{ "ok": true, "uptime_ms": ... }`).
- [ ] `HTTP`: добавить логирование клиента/URI (в debug режиме) для быстрой диагностики “не открывается”.
- [ ] `Docs`: в `docs/api.md` пометить что `POST /api/devices` временный.

## MVP-1 (реальные Zigbee устройства → реестр → UI)

- [ ] Вынести Zigbee управление в отдельный компонент `components/gw_zigbee/`:
  - очередь команд,
  - публикация событий в `gw_core`.
- [ ] На событиях Zigbee обновлять `gw_device_registry` (uid/name/caps/short_addr/last_seen).
- [ ] `GET /api/devices` должен отражать реальные Zigbee устройства (не “ручные”).

## MVP-2 (управление сетью и устройствами)

- [ ] `POST /api/network/permit_join` (из `docs/architecture.md`) и кнопка в UI.
- [ ] `POST /api/devices/{device_uid}/actions/onoff` (on/off/toggle).

## MVP-3 (события и отладка)

- [ ] SSE `GET /api/events` (хотя бы последние N событий).
- [ ] Корреляция запрос→результат (`correlation_id`) для действий из UI.

## Позже

- [ ] `gw_storage`: NVS/FS для устройств/правил/конфига UI.
- [ ] `gw_mqtt`: мост событий наружу (без брокера на устройстве).
- [ ] Rule engine MVP.

