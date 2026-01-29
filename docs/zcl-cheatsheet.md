# ZigBee Cluster Library (ZCL6, 07-5123-06) — памятка для gateway/автоматизаций

Источник: `docs/07-5123-06-zigbee-cluster-library-specification.pdf` (ZCL revision 6, 14 Jan 2016).

Цель: быстро ориентироваться в **кластерах, командах, атрибутах и единицах измерения** для нашего gateway:
- нормализованные события (`zigbee.command`, `zigbee.attr_report`)
- state store (температура/влажность/бат.)
- автодискавери “switch vs relay/light”

---

## 1) ZCL: что важно знать (минимум)

### 1.1 Frame basics
ZCL кадр всегда содержит:
- **Frame Control** (тип: global или cluster-specific; флаги manufacturer-specific, direction, disable default response)
- (опционально) **Manufacturer Code** (если manufacturer-specific=1)
- **Transaction Sequence Number (TSN)** (1 байт)
- **Command Identifier** (1 байт)
- Payload

Практика:
- глобальные команды (чтение/репорт/конфиг репорта) — это “Foundation”
- команды типа `toggle/on/off/level` — это cluster-specific

### 1.2 Default Response (почему иногда “тишина”)
Если `Disable Default Response = 1`, то устройство не обязано присылать Default Response на успех, только на ошибки.

---

## 2) Foundation commands (то, на чём держатся репорты)

Это основа для сенсоров и состояния (наши `zigbee.attr_report`).

### 2.1 Configure Reporting / Report Attributes (концепция)
- **Report Attributes** — сервер “пушит” изменения атрибутов клиенту.
- **Configure Reporting** задаёт правила репортинга:
  - `min_interval`, `max_interval`
  - `reportable_change` (для “analog” типов)

Нюансы:
- `max_interval = 0x0000` → периодических репортов нет, но change-based возможен.
- `max_interval = 0xFFFF` → репорты **не генерируются**.
- Для **discrete** типов репорт “на любое изменение”.
- Для **analog** типов репорт “если |delta| >= reportable_change”.

---

## 3) “Кластера, которые нужны нам сейчас” (IDs, команды, атрибуты)

### 3.1 Device Configuration & Installation (глава “General”)
Список кластеров (ID):
- `0x0000` **Basic**
- `0x0001` **Power Configuration**
- `0x0003` **Identify**

#### Basic (0x0000)
Полезно помнить про команду:
- `Reset to Factory Defaults` (command id `0x00`)

Важно: ZCL reset сбрасывает **атрибуты** кластеров на дефолты, но **не должен** стирать ZigBee сеть/биндинги/группы (это отдельно).

#### Power Configuration (0x0001) — батарейка
Атрибуты (Battery Information):
- `0x0020` `BatteryVoltage` (uint8) — в **100mV**, `0xFF` invalid/unknown
- `0x0021` `BatteryPercentageRemaining` (uint8) — в **0.5%**:
  - `0x00` = 0%
  - `0x64` = 50%
  - `0xC8` = 100%
  - `0xFF` invalid/unknown
  - атрибут репортабельный (можно Configure Reporting)

Маппинг в state store (предложение):
- `battery_pct = BatteryPercentageRemaining / 2.0`

### 3.2 Groups & Scenes
Список:
- `0x0004` **Groups**
- `0x0005` **Scenes**

#### Groups (0x0004)
Идея: endpoint можно добавить в одну/несколько **групп** (group id 16-bit, примерно `0x0001–0xFFF7`). Команды, отправленные groupcast’ом (APS `DstAddrMode=0x01`), доставляются всем endpoint’ам, которые состоят в группе (для устройств с `macRxOnWhenIdle=TRUE`).

Важно для нас:
- sleeping end device может пропускать groupcast’ы → ему опционально поддерживать Groups/Scenes server.
- группы для исходящих команд у кнопок часто настраиваются **через APS binding**, а не напрямую этим кластером.

Команды (client → server), command id:
- `0x00` Add group — payload `{ group_id:uint16, group_name:string }`
- `0x01` View group — payload `{ group_id:uint16 }`
- `0x02` Get group membership — payload `{ group_count:uint8, group_list:[uint16...] }` (`group_count=0` означает “верните все группы”)
- `0x03` Remove group — payload `{ group_id:uint16 }`
- `0x04` Remove all groups — payload нет
- `0x05` Add group if identifying — payload `{ group_id:uint16, group_name:string }` (без response, обычно multicast/broadcast)

Команды (server → client) — responses:
- `0x00` Add group response — payload `{ status:enum8, group_id:uint16 }`
- `0x01` View group response — payload `{ status:enum8, group_id:uint16, group_name:string }`
- `0x02` Get group membership response — payload `{ capacity:uint8, group_count:uint8, group_list:[uint16...] }`
- `0x03` Remove group response — payload `{ status:enum8, group_id:uint16 }`

Нюанс: если Add/View/Remove пришли groupcast’ом или broadcast’ом — **ответ не должен отправляться**.

#### Scenes (0x0005)
Идея: “сцена” = сохранённые значения атрибутов нескольких кластеров на endpoint’е. Обычно сцены привязаны к group id.

Зависимость: endpoint, который реализует **Scenes server**, должен реализовать **Groups server**.

Сцена без группы:
- допускается `group_id=0x0000`, но тогда команды со сценами должны быть **только unicast** (чтобы не словить коллизии `scene_id`).
- `group_id=0x0000` + `scene_id=0x00` зарезервированы как **global scene** (используется в On/Off).

Команды (client → server), command id:
- `0x00` Add Scene
- `0x01` View Scene
- `0x02` Remove Scene
- `0x03` Remove All Scenes
- `0x04` Store Scene
- `0x05` Recall Scene
- `0x06` Get Scene Membership
- `0x40` Enhanced Add Scene (optional)
- `0x41` Enhanced View Scene (optional)
- `0x42` Copy Scene (optional)

Ключевой payload (пример):
- Add Scene payload включает `{ group_id:uint16, scene_id:uint8, transition_time:uint16, scene_name:string, extension_field_sets:[...] }`
- View/Remove обычно `{ group_id:uint16, scene_id:uint8 }`
- Recall Scene обычно `{ group_id:uint16, scene_id:uint8 }`

Нюанс: при groupcast/broadcast для Add/View/Remove/RemoveAll/Store/EnhancedAdd/EnhancedView/Copy — **ответ не должен отправляться**.

### 3.3 On/Off и Level Control (главные для кнопок/реле)
Список:
- `0x0006` **On/Off**
- `0x0007` **On/Off Switch Configuration**
- `0x0008` **Level Control**

#### On/Off (0x0006)
Server attributes:
- `0x0000` `OnOff` (bool) — reportable

Команды (client → server):
- `0x00` Off
- `0x01` On
- `0x02` Toggle
- `0x40` Off with effect (optional)
- `0x41` On with recall global scene (optional)
- `0x42` On with timed off (optional)

Нормализованное событие для автоматизаций (на gateway):
`zigbee.command` payload:
```json
{ "cluster":"0x0006", "cmd":"toggle|on|off", "endpoint": 1 }
```

#### On/Off Switch Configuration (0x0007)
Ключевой атрибут:
- `0x0010` `SwitchActions` (enum8):
  - `0x00` генерировать `On`/`Off`
  - `0x01` генерировать `Off`/`On`
  - `0x02` генерировать `Toggle`/`Toggle`

Практика: если у кнопки `SwitchActions=Toggle`, то это “чистая кнопка toggle”.

#### Level Control (0x0008)
Server attributes (ключевые):
- `0x0000` `CurrentLevel` (uint8) — reportable
- `0x0001` `RemainingTime` (uint16)
- `0x0010` `OnOffTransitionTime` (uint16, 1/10s)
- `0x0011` `OnLevel` (uint8, `0xFF` = “not set”)

Команды (client → server):
- `0x00` Move to Level
- `0x01` Move
- `0x02` Step
- `0x03` Stop
- `0x04` Move to Level (with On/Off)
- `0x05` Move (with On/Off)
- `0x06` Step (with On/Off)
- `0x07` Stop (одинаковый, но “в ветке with On/Off”)

Важное поведение:
- “with On/Off” команды обязаны менять `OnOff` в `0x0006` на том же endpoint (включать перед ростом уровня, выключать при падении до минимума).

Нормализованное событие:
`zigbee.command` payload (пример):
```json
{ "cluster":"0x0008", "cmd":"move_to_level", "endpoint": 1, "args": { "level": 128, "transition_ds": 10 } }
```

### 3.4 Measurement & Sensing (сенсоры)
Список:
- `0x0402` **Temperature Measurement**
- `0x0405` **Relative Humidity Measurement**

#### Temperature Measurement (0x0402)
Атрибуты (сервер):
- `0x0000` `MeasuredValue` (int16) — **0.01°C**: `value = 100 * °C`
  - `0x8000` = invalid
- `0x0003` `Tolerance` (uint16) — репортабельный

Нормализованное событие:
`zigbee.attr_report` payload:
```json
{ "cluster":"0x0402", "attr":"0x0000", "endpoint": 1, "value": 2330, "unit":"cC" }
```

Маппинг в state store (предложение):
- `temperature_c = MeasuredValue / 100.0`

#### Relative Humidity Measurement (0x0405)
Атрибуты (сервер):
- `0x0000` `MeasuredValue` (uint16) — **0.01%**: `value = 100 * %RH`
  - `0xFFFF` = invalid
- `0x0003` `Tolerance` (uint16)

Маппинг:
- `humidity_pct = MeasuredValue / 100.0`

---

## 4) Как определять “тип устройства” для UI/автоименования

Это не “ZCL device type”, а практическая классификация по простому описателю (Simple Descriptor):
- Если endpoint имеет **On/Off client** (`0x0006` в out clusters) → скорее всего **switch/button**.
- Если endpoint имеет **On/Off server** (`0x0006` в in clusters) → скорее всего **relay/light**.
- Если есть `0x0402/0x0405` server → **sensor**.

Дополнительно можно уточнять по `profile_id`/`device_id` (ZigBee Device IDs), но это уже вне одного ZCL PDF и зависит от профиля (ZHA/ZLL/…).
