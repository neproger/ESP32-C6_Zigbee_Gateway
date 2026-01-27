# Файловая структура проекта (целевой вид)

Цель: не складывать всё в `main/esp_zigbee_gateway.c`, а разнести по компонентам ESP-IDF с понятными границами и
зависимостями.

## Общая схема каталогов

```
.
├─ main/                      # Минимальный glue-код: app_main(), wiring компонентов
├─ components/
│  ├─ gw_core/                # Ядро: шина событий, модели данных, реестр, правила (без Wi‑Fi/Zigbee)
│  │  ├─ include/gw_core/
│  │  └─ src/
│  ├─ gw_zigbee/              # Zigbee Manager: join, discovery, raw/normalized events, очередь команд
│  │  ├─ include/gw_zigbee/
│  │  └─ src/
│  ├─ gw_http/                # REST API + Web UI + SSE
│  │  ├─ include/gw_http/
│  │  └─ src/
│  ├─ gw_mqtt/                # MQTT client bridge (опционально)
│  │  ├─ include/gw_mqtt/
│  │  └─ src/
│  └─ gw_storage/             # Persistence (NVS/FS): устройства, правила, конфиг UI
│     ├─ include/gw_storage/
│     └─ src/
├─ docs/                      # Документация (архитектура, API, форматы)
└─ partitions.csv
```

## Правила зависимостей (важно)

- `gw_core` ни от чего “сетевого/железного” не зависит (максимум `esp_event`, `log`, `freertos`).
- `gw_zigbee` зависит от `gw_core` и Zigbee SDK/IDF компонентов.
- `gw_http` зависит от `gw_core` (и часто от `gw_storage`) и `esp_http_server`.
- `gw_mqtt` зависит от `gw_core` и `mqtt`.
- `gw_storage` зависит от `nvs_flash` (+ опционально `spiffs`/`littlefs`).

Так мы избегаем циклических зависимостей и можем тестировать/менять части отдельно.

## Что лежит в `main/`

`main/` должен остаться тонким:
- инициализация NVS/NETIF/loop
- старт `gw_core`, затем `gw_storage`, `gw_zigbee`, `gw_http` (и опционально `gw_mqtt`)
- “склейка” конфигов и порядок запуска

## Разделение исходников внутри компонентов

Пример для `gw_core/src/`:
- `event_bus.c` — обертка над `esp_event`
- `device_registry.c` — in-memory модель устройств + события обновлений
- `rules_engine.c` — матчинг событий и генерация действий
- `types.c` — утилиты, парсинг/форматирование UID и т.п.

## Где держать формат API и событий

- Спецификация internal events: `docs/architecture.md`
- Спецификация REST API: `docs/architecture.md` (позже вынести в `docs/api.md`)
- Версия форматов (если нужно): `docs/protocol.md`

