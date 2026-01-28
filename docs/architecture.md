# Целевая архитектура (ESP32‑C6 Zigbee Gateway + Web UI + Автоматизации)

Этот документ описывает целевую архитектуру: как развить текущий пример Zigbee gateway в небольшой автономный шлюз
с автоматизациями:

- Zigbee сеть (координатор)
- Локальная Web UI + REST API
- Event bus (всё превращаем в события)
- Движок правил (автоматизации)
- Опциональная интеграция через MQTT **клиент** (брокер внешний)

## Статус реализации (as-is)

Состояние репозитория на данный момент:

- Реализовано: базовый `gw_core` (реестр устройств), `gw_http` (минимальный UI + `GET/POST /api/devices`), Wi‑Fi STA с попыткой подключения к известным точкам (см. `main/wifi_aps_config.h`), печать IP и ссылки на UI.
- Не реализовано (в архитектуре описано как “целевое”): `gw_zigbee` как отдельный менеджер/очередь команд, `gw_storage`, `gw_mqtt`, SSE `/api/events`, правила и полноценный API.

Связанные документы:
- Быстрый старт: `docs/getting-started.md`
- Индекс: `docs/index.md`
- Текущее состояние (as-is): `docs/current-state.md`
- API (as-is): `docs/api.md`
- Troubleshooting: `docs/troubleshooting.md`
- План работ: `docs/roadmap.md`

## Цели (MVP)

- Подключать (pair/join) Zigbee устройства из Web UI (“разрешить присоединение / permit join”).
- Вести реестр устройств (стабильные ID, имена, capabilities).
- Публиковать все входящие/исходящие действия как события.
- Настраивать простые автоматизации: *когда событие совпало → выполнить действие*.
- Дать базовое управление устройствами (On/Off, Toggle; позже Level, Scenes и т.д.).

## Что НЕ делаем сначала

- Не запускаем MQTT брокер на ESP32‑C6 (используем внешний брокер).
- Не обещаем “как Zigbee2MQTT/ZHA” на 100% устройств и все vendor quirks с первого дня.
- Не строим большую платформу умного дома — это сфокусированный шлюз.

## Компоненты верхнего уровня

```mermaid
flowchart LR
  UI[Web UI] <---> API[REST API]
  API --> BUS[Event Bus]
  ZB[Zigbee Manager] <--> BUS
  RULES[Rule Engine] <--> BUS
  REG[Device Registry + Persistence] <--> ZB
  REG <--> RULES
  MQTT[MQTT Client (optional)] <--> BUS
```

### 1) Zigbee Manager (менеджер Zigbee)

Задачи:
- Инициализация Zigbee стека, формирование сети, управление `permit_join`.
- Ведение актуальных адресов в сети (short address) и discovery endpoints.
- Превращение входящих Zigbee событий (ZCL, reports, commands) в события `zigbee.raw`.
- Где возможно — нормализация в “смысловые” события (например `zigbee.button`, `zigbee.onoff`).
- Выполнение исходящих действий (ZCL команды) строго в **одном контексте выполнения Zigbee** (task/queue).

Ключевое ограничение:
- Считать Zigbee стек “однопоточным”: задачи не Zigbee (HTTP, правила) не должны дергать API стека напрямую.
  Они отправляют команду в очередь Zigbee Manager, а тот выполняет и публикует результат как событие.

### 2) Event Bus (шина событий)

Задачи:
- Центральная pub/sub шина внутри прошивки.
- Все важные изменения состояния становятся событиями:
  - входящий Zigbee: `zigbee.raw`, `zigbee.button`, `zigbee.attribute_report`, ...
  - исходящие запросы: `api.set_onoff`, `rules.action`, ...
  - результаты: `zigbee.cmd_result`, `api.response`, ...
  - системные: `system.timer`, `system.boot`, `system.wifi`, ...

Рекомендации по реализации:
- Использовать `esp_event` как легкую основу pub/sub и добавить отдельную очередь/таск “automation worker” для тяжелых задач.
- Фильтрацию подписок (по `type`, `device_uid`, `endpoint`, `cluster`, `command`, ...) делать на уровне Rule Engine,
  даже если базовая шина глобальная.

### 3) Реестр устройств + сохранение (Persistence)

Задачи:
- Ведение стабильной идентичности устройства и пользовательских метаданных.
- Хранение обнаруженных возможностей и runtime-маппинга:
  - **Стабильный** `device_uid` = Zigbee IEEE (EUI‑64) адрес.
  - **Динамический** `short_addr` = текущий короткий адрес в сети (может меняться после rejoin).
- Метаданные для UI: `name`, `room`, `hidden`, `icon`, `last_seen`.
- Capabilities: какие действия UI и какие нормализации событий возможны для устройства.

Сохранение:
- NVS key-value для MVP (JSON blob на устройство и на правило).
- Опционально позже: LittleFS/SPIFFS для больших конфигов и ассетов UI.

### 4) Rule Engine (автоматизации)

Задачи:
- Подписка на Event Bus.
- Матчинг входящих событий по правилам:
  - триггер может быть конкретным (`device_uid == X`) или глобальным (“любое устройство”)
  - условия могут проверять значения в payload
- Выполнение действий:
  - опубликовать новое событие (`rules.action`)
  - запросить Zigbee команду через очередь Zigbee Manager
  - опционально опубликовать в MQTT (клиент)

Модель правила (MVP):
- `enabled`
- `trigger` (фильтр события)
- опционально `conditions`
- `actions[]` (одно или несколько)

### 5) REST API + Web UI

Задачи:
- Дать пользователю:
  - кнопку “permit join” с таймером/фидбеком
  - список устройств и страницу устройства
  - просмотр live событий (для дебага)
  - список правил / редактор правил
  - базовое управление on/off для устройств с соответствующей capability

Рекомендации по реализации:
- `esp_http_server` для HTTP.
- Статический UI либо как встроенные ассеты (на этапе сборки), либо из FS.
- JSON REST endpoints; сначала без websockets; при необходимости добавить Server-Sent Events (SSE) для live потока событий.

### 6) MQTT Client (опционально)

Задачи:
- Бриджить выбранные события в MQTT topics и принимать входящие MQTT команды как события.
- MQTT брокер внешний (Mosquitto/EMQX/Home Assistant add-on).

Замечания по “потянет ли”:
- ESP32‑C6 нормально тянет MQTT клиента; TLS заметно увеличивает RAM/CPU.
- Большой трафик Wi‑Fi может ухудшать Zigbee на однурадиотрактных чипах; лучше низкие скорости сообщений.

## Базовые модели данных (MVP)

### Event (нормализованное внутреннее представление)

Поля (концептуально):
- `type`: string (`"zigbee.raw"`, `"zigbee.button"`, `"api.set_onoff"`, ...)
- `ts_ms`: uint64
- `source`: string (`"zigbee" | "api" | "rules" | "system" | "mqtt"`)
- `device_uid`: string (EUI‑64 в hex, например `"0x00124B0012345678"`) optional
- `endpoint`: uint8 optional
- `payload`: JSON-подобная map (или C struct + опциональная JSON сериализация)
- `correlation_id`: string optional (трассировка request→result)

Два уровня:
- `zigbee.raw`: всегда публикуем с cluster/command/attr IDs.
- `zigbee.*` смысловые события: публикуем только если уверенно нормализовали.

### Device (устройство)

- `device_uid` (IEEE, стабильный)
- `short_addr` (текущий)
- `endpoints[]` и `clusters` (обнаруженные)
- `manufacturer`, `model` (Basic cluster, если доступно)
- `capabilities[]` (выведенные + опциональный ручной override)
- `name`, `room`, `last_seen`

### Capability (минимальный набор, предложение)

- `onoff` (умеет `On/Off/Toggle`)
- `level` (яркость)
- `button` (генерирует события нажатий)
- `sensor` (температура/влажность/и т.п.)
- `power` (метринг)

## Черновик REST API

Все endpoints возвращают JSON.

- `POST /api/network/permit_join` body: `{ "seconds": 180 }`
- `POST /api/network/close` (optional)
- `GET  /api/devices`
- `GET  /api/devices/{device_uid}`
- `PATCH /api/devices/{device_uid}` body: `{ "name": "...", "room": "...", "capability_overrides": [...] }`
- `POST /api/devices/{device_uid}/actions/onoff` body: `{ "state": "on" | "off" | "toggle" }`
- `GET  /api/rules`
- `POST /api/rules`
- `PATCH /api/rules/{id}`
- `DELETE /api/rules/{id}`
- `GET  /api/events` (optional SSE: stream recent events)

## Контексты выполнения и конкуррентность

- Zigbee stack: принадлежит Zigbee task; все взаимодействия идут через Zigbee Manager.
- HTTP server: свои task(и); не должен напрямую дергать Zigbee stack.
- Rule engine: потребляет события; отправляет действия в Zigbee Manager через очередь; публикует события обратно.
- Запись реестра: сериализовать, чтобы не бодаться за NVS; дебаунсить частые обновления (например `last_seen`).

## Вехи (milestones)

1) Event Bus + in-memory реестр (пока только лог)
2) Zigbee join + сохранение реестра (NVS)
3) REST API + минимальный UI (permit join, список, on/off)
4) Rule engine MVP (триггер → on/off)
5) Поток событий в UI (SSE) + UI редактор правил
6) Опциональный MQTT client bridge

## Практические уточнения и подводные камни

### Zigbee: контекст выполнения и очередь команд

- Zigbee стек должен жить в одном контексте (таск/loop). Любые команды “извне” (HTTP, правила) — только через очередь
  команд Zigbee Manager.
- Модель “команда → действие → событие результата” по сути является lightweight actor model и хорошо масштабируется.

### Нормализация событий: «если не уверены — raw»

- Самая “грязная” часть — кнопки и vendor-specific устройства: разные endpoints, разные ZCL команды/кластеры, разные payload.
- Правило MVP: если нет уверенности в интерпретации — публикуем `zigbee.raw` и (опционально) не публикуем `zigbee.button`.

### Event Bus: размер событий и память

- Внутреннее представление событий лучше держать как C struct (минимум heap-аллоков), а JSON генерировать только на
  границе (REST/MQTT/Debug).
- Полезно разделить обработку на “быструю” и “медленную”:
  - fast: Zigbee/Rules/Timers (не должно блокироваться Wi‑Fi/UI)
  - slow: мосты наружу (REST/MQTT/SSE), где возможны задержки

### Реестр устройств: rejoin и идентификаторы

- `device_uid` должен быть IEEE (EUI‑64) — это стабильный ключ.
- `short_addr` может меняться при rejoin — хранить как текущий runtime-параметр, обновлять при появлении устройства.

### Capabilities: откуда “правда”

Реалистичная стратегия для MVP:
- derive из кластеров/эндпоинтов (автоматически) + возможность ручного override в UI.
- vendor quirks (драйверы) добавить точечно позже, по мере появления конкретных устройств.

### Rule Engine: фильтры и корреляция

- Триггер удобно описывать как фильтр по полям события (включая значения в payload), например:

```json
{
  "type": "zigbee.button",
  "device_uid": "0x00124B0012345678",
  "payload.event": "single"
}
```

- Для трассировки цепочек полезен `correlation_id`: `api → rules.action → zigbee.cmd → zigbee.cmd_result`.

### Web UI: почему SSE стоит сделать рано

- Live-поток событий критичен для отладки и понимания, что реально прилетает от устройств.
- Даже минимальный вариант (SSE `/api/events`) сильно повышает удобство разработки.

### MQTT: клиент да, брокер нет

- MQTT брокер на ESP32‑C6 как “продуктовое решение” обычно нецелесообразен; лучше внешний брокер.
- На C6 Wi‑Fi и Zigbee делят радио, поэтому при большой MQTT нагрузке может страдать Zigbee:
  - QoS 0 где можно
  - ограничивать частоту публикаций
  - батчить/дедуплицировать сообщения

### Рекомендованная последовательность реализации (перестановка вех)

Более “естественный” порядок для разработки и отладки:
1) Event Bus + логирование событий
2) Zigbee join + `zigbee.raw` события
3) Реестр устройств + сохранение (NVS)
4) REST API + минимальный UI
5) Исходящие команды On/Off
6) Rule Engine MVP
7) SSE live events в UI
8) MQTT client bridge (опционально)

### Вопросы, которые лучше зафиксировать отдельно (позже)

- Security: нужен ли пароль на UI, HTTPS, ограничения по сети.
- OTA: нужен ли механизм обновления.
- Backup/Import: экспорт/импорт устройств и правил (JSON).

## Связанные документы

- Файловая структура: `docs/file-structure.md`
