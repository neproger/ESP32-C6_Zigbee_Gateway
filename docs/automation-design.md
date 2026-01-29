# Автоматизации (to-be дизайн)

Цель: сделать простые и надёжные автоматизации (в стиле Home Assistant), но **в JSON**, чтобы один и тот же формат можно было использовать:
- в Web UI,
- через WebSocket (`docs/ws-protocol.md`),
- в будущем на других клиентах (например, второй ESP32 с дисплеем),
- и внутри ESP (engine на C).

Ключевые принципы:
- **Автоматизации ссылаются на устройства по `device_uid` (IEEE)**, а не по имени. Имена можно менять без поломки правил.
- События для автоматизаций должны быть **нормализованы** (структурированы), а не “человекочитаемые строки”.
- Один `event_bus`, много типов событий.

---

## 1) Идентификаторы устройств и имена

### `device_uid`
Стабильный ключ устройства: строка вида `0x00124B0012345678` (из IEEE).

### `name`
Человекочитаемое имя (“switch1”, “relay1” и т.п.).

Правило: **в хранилище и автоматизациях только `device_uid`**, а `name` — только для отображения/редактирования.

---

## 2) Нормализованные события

Сейчас события публикуются как `type/source/msg` (строки). Для автоматизаций вводим нормализованные `type` и структурированный `payload`.

### Event envelope (целевая форма)

```json
{
  "v": 1,
  "ts_ms": 1730000000000,
  "type": "zigbee.command",
  "source": "zigbee",
  "device_uid": "0x00124B0012345678",
  "short_addr": 1117,
  "payload": { "cmd": "toggle", "endpoint": 1, "cluster": "0x0006" }
}
```

Примечания:
- `v` — версия схемы события (для совместимости клиентов).
- `payload` — объект, зависящий от `type`.

### Как это реализовано сейчас (MVP, без ломания UI)
- В `event_bus` мы всё ещё храним `msg` как строку.
- Для нормализованных событий `msg` хранит **JSON payload** (строка вида `{...}`).
- В WebSocket `/ws` для таких событий дополнительно приходит поле `payload` (объект), а `msg` остаётся строкой для отображения/логов.

### Базовые типы событий (MVP)

#### `zigbee.command`
Команды, которые пришли в координатор (например, кнопка отправила `toggle`):

```json
{ "cmd": "toggle|on|off|move_to_level", "endpoint": 1, "cluster": "0x0006|0x0008", "args": { } }
```

#### `zigbee.attr_report`
Отчёты атрибутов (сенсоры/состояния):

```json
{ "endpoint": 1, "cluster": "0x0402", "attr": "0x0000", "value": 2150, "unit": "cC" }
```

#### `device.join` / `device.leave`
Сетевые события (join/rejoin/leave):

```json
{ "rejoin": true }
```

#### `timer.tick`
Событие таймера для расписаний (генерируем локально на gateway):

```json
{ "cron": "* * * * *", "tz": "UTC" }
```

### Инкрементальный переход (не ломая UI)
Пока `event_bus` хранит строки, можно договориться:
- для “нормализованных” типов `msg` — это JSON-строка `payload` (или целиком envelope),
- для legacy типов `msg` остаётся текст.

---

## 3) Формат автоматизации (JSON)

Автоматизация = `triggers` → `conditions` → `actions`.

```json
{
  "v": 1,
  "id": "auto_1",
  "name": "Кнопка + температура",
  "enabled": true,
  "triggers": [
    { "type": "event", "event_type": "zigbee.command", "match": { "device_uid": "0xAAA...", "payload.cmd": "toggle" } }
  ],
  "conditions": [
    { "type": "state", "op": ">", "ref": { "device_uid": "0xBBB...", "key": "temperature_c" }, "value": 10 }
  ],
  "actions": [
    { "type": "zigbee", "cmd": "on", "device_uid": "0xCCC...", "endpoint": 1, "cluster": "0x0006" }
  ],
  "mode": "single"
}
```

### Поля (MVP)
- `v`: версия схемы автоматизаций.
- `id`: строковый идентификатор (уникальный).
- `enabled`: включена/выключена.
- `triggers`: список триггеров (MVP — `event` и `timer`).
- `conditions`: список условий (MVP — `state` сравнение).
- `actions`: список действий (MVP — Zigbee команды и “виртуальные” действия).
- `mode`: как вести себя при частых срабатываниях:
  - `single` — игнорировать новые, пока выполняется текущая,
  - (позже) `restart`, `queued`.

---

## 4) State store (для conditions)

Чтобы условия типа “температура > 10” работали, gateway должен хранить “последнее известное состояние”:
- при `zigbee.attr_report` обновляем `state[device_uid][key]`,
- автоматизации читают состояние только из state store (быстро и стабильно).

MVP ключей (предложение):
- `onoff` (bool),
- `temperature_c` (float),
- `humidity_pct` (float),
- `battery_pct` (uint),
- `last_seen_ms` (uint64).

---

## 5) Хранение и API

### Хранение
Автоматизации храним как JSON (opaque) в NVS (через `gw_automation_store`).

Правило совместимости:
- новый код должен уметь читать старые `v` и мигрировать (или мягко отключать).

### Управление (предпочтительно через WS)
См. `docs/ws-protocol.md`:
- `automations.list`
- `automations.put`
- `automations.remove`
- `automations.set_enabled`

Дополнительно (опционально):
- `devices.set_name` (или текущий `POST /api/devices?...`) для переименований.

---

## 6) Что считаем “готовым MVP”

1) Нормализовать минимум событий: `zigbee.command` (toggle/on/off), `zigbee.attr_report` (temp/hum).
2) State store с 2–3 ключами (температура/влажность/last_seen).
3) Automation engine: `event` trigger + `state` conditions + `zigbee` action.
4) UI: список автоматизаций, enable/disable, просмотр JSON.
