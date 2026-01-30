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

## Важно: `payload.cluster` / `payload.attr` — что это и зачем

Это поля из ZCL‑мира, и они зависят от типа события:

### `zigbee.command`
Это “пришла ZCL команда” (например On/Off Toggle).

Обычно достаточно:
- `device_uid` (кто прислал)
- `payload.endpoint` (с какого endpoint)
- `payload.cluster` (в каком кластере; например `0x0006`)
- `payload.cmd` (какая команда; например `toggle`)

`payload.attr` здесь **не обязателен** и в общем случае не имеет смысла: команда ≠ изменение атрибута.

Пример:
```json
{ "type":"zigbee.command", "device_uid":"0x...", "payload":{ "endpoint":1, "cluster":"0x0006", "cmd":"toggle" } }
```

### `zigbee.attr_report`
Это “пришёл репорт значения атрибута” (Foundation: Report Attributes).

Здесь `payload.attr` как раз критичен:
- `payload.cluster` (например `0x0402` Temperature Measurement)
- `payload.attr` (например `0x0000` MeasuredValue)
- `payload.value` (raw значение, как в Zigbee)
- `payload.unit` (наша строка для удобства/логов)

Пример:
```json
{ "type":"zigbee.attr_report", "device_uid":"0x...", "payload":{ "endpoint":3, "cluster":"0x0402", "attr":"0x0000", "value":2330, "unit":"cC" } }
```

Таблица “что часто встречается” (см. `docs/zcl-cheatsheet.md`):
- `0x0006` On/Off: attr `0x0000` OnOff (bool)
- `0x0008` Level Control: attr `0x0000` CurrentLevel (0..254)
- `0x0402` Temperature: attr `0x0000` MeasuredValue (int16, 0.01°C)
- `0x0405` Humidity: attr `0x0000` MeasuredValue (uint16, 0.01%RH)

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
- Для нормализованных событий gateway дополнительно хранит **structured payload** (JSON-строка), а `msg` остаётся строкой для отображения/логов.
- В WebSocket `/ws` и в `GET /api/events` для таких событий дополнительно приходит поле `payload` (объект), а `msg` остаётся строкой.

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
    { "type": "zigbee", "cmd": "onoff.on", "device_uid": "0xCCC...", "endpoint": 1 }
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

### Actions: Zigbee-примитивы (не изобретаем велосипед)
Идея: **actions в итоге сводятся к Zigbee Groups/Scenes/Binding или к device unicast-командам**.
Это даёт максимум совместимости (устройства умеют это нативно), а “умность” (условия/таймеры/AND/OR) остаётся в rules engine.

#### 1) Команды на конкретное устройство (unicast)
```json
{ "type": "zigbee", "cmd": "onoff.toggle", "device_uid": "0x00124B0012345678", "endpoint": 1 }
```

```json
{ "type": "zigbee", "cmd": "level.move_to_level", "device_uid": "0x00124B0012345678", "endpoint": 1, "level": 254, "transition_ms": 500 }
```

```json
{ "type": "zigbee", "cmd": "color.move_to_color_xy", "device_uid": "0x00124B0012345678", "endpoint": 1, "x": 30000, "y": 30000, "transition_ms": 500 }
```

```json
{ "type": "zigbee", "cmd": "color.move_to_color_temperature", "device_uid": "0x00124B0012345678", "endpoint": 1, "mireds": 250, "transition_ms": 500 }
```

#### 2) Команды на группу (groupcast)
Если нужно управлять “комнатой”/набором устройств — лучше слать в группу (меньше трафика и проще логика).
```json
{ "type": "zigbee", "cmd": "onoff.off", "group_id": "0x0003" }
```

#### 3) Сцены (Scenes cluster)
Сцена — это “preset” состояния группы. Очень удобно вместо ручного набора из 10 действий.
```json
{ "type": "zigbee", "cmd": "scene.store", "group_id": "0x0003", "scene_id": 1 }
```

```json
{ "type": "zigbee", "cmd": "scene.recall", "group_id": "0x0003", "scene_id": 1 }
```

#### 4) Binding / Unbinding (автономный режим)
Binding создаёт прямую связь (например, кнопка -> лампа или кнопка -> gateway) **без участия gateway в рантайме**.
Это “автоматизация на стороне Zigbee”, полезно для low-latency и работы при перезагрузке gateway.

```json
{ "type": "zigbee", "cmd": "bind", "src_device_uid": "0xAAA...", "src_endpoint": 1, "cluster_id": "0x0006", "dst_device_uid": "0xBBB...", "dst_endpoint": 1 }
```

```json
{ "type": "zigbee", "cmd": "unbind", "src_device_uid": "0xAAA...", "src_endpoint": 1, "cluster_id": "0x0006", "dst_device_uid": "0xBBB...", "dst_endpoint": 1 }
```

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
Автоматизации храним в выделенном SPIFFS-разделе `gw_data`, смонтированном в `/data`:
- `/data/autos.bin` — список автоматизаций (id/name/enabled/json) для UI/редактирования
- `/data/<automation_id>.gwar` — compiled бинарник для выполнения (runtime)

Инвариант архитектуры: rules engine исполняет только `.gwar` (никакого “исполнения JSON” на каждое событие).

Важно: при `automations.put`/enable компиляция должна пройти успешно, иначе сохранение отклоняется
(чтобы не получалось “в UI сохранилось, но по триггеру не работает”).

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

1) Нормализовать минимум событий: `zigbee.command` (toggle; позже on/off), `zigbee.attr_report` (temp/hum).
2) State store с 2–3 ключами (температура/влажность/last_seen).
3) Automation engine: `event` trigger + `state` conditions + `zigbee` action.
4) UI: список автоматизаций, enable/disable, просмотр JSON.

---

## Реальность “as-is”: что компилируется в `.gwar` прямо сейчас

UI хранит/редактирует JSON, но rules engine исполняет только compiled `.gwar`.

### Поддержано (MVP)
- triggers: `event` с `event_type` одним из: `zigbee.command`, `zigbee.attr_report`, `device.join`, `device.leave`
- conditions: `state` сравнения (AND по списку)
- actions (Zigbee primitives, без runtime JSON парсинга):
  - device on/off: `{ "type":"zigbee", "cmd":"onoff.on|off|toggle", "device_uid":"0x...", "endpoint": 1 }`
  - device level: `{ "type":"zigbee", "cmd":"level.move_to_level", "device_uid":"0x...", "endpoint": 1, "level": 0..254, "transition_ms": 0..60000 }`
  - group on/off: `{ "type":"zigbee", "cmd":"onoff.on|off|toggle", "group_id":"0x0003" }`
  - group level: `{ "type":"zigbee", "cmd":"level.move_to_level", "group_id":"0x0003", "level": 0..254, "transition_ms": 0..60000 }`
  - scenes: `{ "type":"zigbee", "cmd":"scene.store|scene.recall", "group_id":"0x0003", "scene_id": 1..255 }`
  - binding: `{ "type":"zigbee", "cmd":"bind|unbind", "src_device_uid":"0x...", "src_endpoint": 1..240, "cluster_id":"0x0006", "dst_device_uid":"0x...", "dst_endpoint": 1..240 }`

### Запланировано (следующий шаг “укрепления”)
Расширять компиляцию действий (не меняя UI‑JSON формат) на:
- color control (device/group): `color.move_to_color_xy`, `color.move_to_color_temperature`
- “management” действия: permit join, leave/kick, mgmt bind table readback (как инструменты админки)
