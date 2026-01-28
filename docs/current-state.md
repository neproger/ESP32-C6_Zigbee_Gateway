# Текущее состояние (as-is)

Цель этого документа — чтобы было быстро понятно:
1) что уже работает,
2) где это в коде,
3) что дальше логично делать.

## Что уже работает

### Wi‑Fi STA (несколько SSID)

- Код: `main/gw_wifi.c`
- Конфиг (локальный, не коммитится): `main/wifi_aps_config.h`
- Шаблон: `main/wifi_aps_config.example.h`

Поведение:
- Сканирует доступные AP.
- Выбирает “известные” SSID (из конфига), сортирует по RSSI, подключается.
- После получения IP печатает:
  - `Got IP: ...`
  - `Web UI: http://<ip>/`

### HTTP сервер + простой UI

- Код: `components/gw_http/src/gw_http.c`
- Публичный API: `components/gw_http/include/gw_http/gw_http.h`

Эндпоинты:
- UI: `GET /`
- API: `GET /api/devices`, `POST /api/devices?...`

Детали: `docs/api.md`.

### Реестр устройств (in-memory)

- Код: `components/gw_core/src/device_registry.c`
- API: `components/gw_core/include/gw_core/device_registry.h`

Примечание: сейчас это in-memory структура для UI/отладки.

### Zigbee (пример/скелет)

- Основной файл: `main/esp_zigbee_gateway.c`

Примечание: сейчас Zigbee логика не вынесена в отдельный `gw_zigbee` менеджер.

## Что сейчас “временно”

- Wi‑Fi живёт в `main/` (по целевой структуре лучше вынести в компонент).
- `POST /api/devices` — временный ручной API для теста UI (не связан с реальными Zigbee discovery).
- Целевой API из `docs/architecture.md` пока не реализован.

## Следующие шаги (коротко)

1) Сделать реальные Zigbee события → обновление реестра (`gw_core`) и отображение в UI.
2) Добавить минимум управления: `permit_join` из UI/API.
3) Стабилизировать протокол API (вынести целевой API из `architecture.md` в отдельный файл).

Полный план: `docs/roadmap.md`.

