# Getting Started (ESP32-C6 Zigbee Gateway)

> Все файлы в `docs/` в UTF‑8.

## Требования

- ESP-IDF `v5.5.1` (проект проверялся на `v5.5.1`).
- Плата `ESP32-C6`.

## Настройка Wi‑Fi (несколько точек)

Файл с паролями **не коммитится**:

1) Открой `main/wifi_aps_config.h`
2) Заполни список `GW_WIFI_APS[]` своими SSID/паролями.

Если у тебя только шаблон — смотри `main/wifi_aps_config.example.h`.

Важно:
- ESP32‑C6 подключается к **2.4 GHz**, 5 GHz сети не подойдут.
- Если SSID скрыт — scan может не найти его.

## Сборка

В терминале ESP‑IDF:

```bash
idf.py set-target esp32c6
idf.py build
```

## Прошивка и монитор

```bash
idf.py -p COMx flash monitor
```

## Web UI

После получения IP в логах печатается ссылка вида:

```
Web UI: http://192.168.1.105/
```

Если страница не открывается, см. `docs/troubleshooting.md`.

