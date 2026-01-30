# Roadmap / Checklist

Этот файл — “что делать дальше” в виде последовательного чеклиста.

Примечание: проект пока non-prod, поэтому совместимость форматов можно ломать (проще пересоздавать конфиги, чем мигрировать).

## Сейчас (стабилизация)

- [ ] `HTTP/WS`: устранить причины паник “Stack protection fault” в task `httpd` (обычно: большие stack-alloc буферы/`snprintf` на больших строках, лишние копии JSON).
- [ ] `Events`: держать payload коротким; большие структуры не логировать целиком в `msg`.
- [ ] `Docs`: поддерживать `docs/ws-protocol.md` и `docs/architecture.md` в актуальном состоянии.

## Automations MVP (уже есть, укреплять)

- [x] Хранение автомаций: `/data/autos.bin` (JSON authoring) + `/data/<id>.gwar` (execution).
- [x] Инвариант: rules engine исполняет только `.gwar` (никакого runtime JSON исполнения).
- [x] UI редактор автоматизаций + enable/disable.
- [ ] Нормализованные события: расширить `zigbee.command` (сейчас стабильно только toggle; добавить `on`/`off` где применимо).
- [ ] State store: поддержать больше ключей из ZCL (см. `docs/zcl-cheatsheet.md`): `current_level`, `onoff`, `battery_pct`, `illuminance_lux`, `occupancy`, ...
- [ ] Стабильная классификация endpoint’ов (switch/light/sensor) на основе Simple Descriptor + кластеров in/out.

## Automations v2 (следующий большой шаг)

- [ ] Компиляция actions: добавить group actions (`groups.onoff`, `groups.level`, …).
- [ ] Компиляция actions: добавить scenes (`scene.store`, `scene.recall`).
- [ ] Компиляция actions: добавить bind/unbind (`bindings.bind`, `bindings.unbind`).
- [ ] Triggers: добавить таймеры (`timer.tick`, cron/interval), debounce/throttle (для кнопок/сенсоров).
- [ ] Conditions: добавить OR/NOT и “группы условий” (простая boolean алгебра).
- [ ] Mode: добавить `restart`/`queued` (сейчас `single`).

## Zigbee (углубление по спецификации)

- [ ] Configure Reporting на сенсорах (min/max/reportable_change) — стабильнее state store и условия.
- [ ] Mgmt_Bind readback (diagnostic): “куда шлёт switch” без гаданий.
- [ ] “Kick/leave”: повторять leave при `Device_annce`, если прошлый leave не дошёл (device offline / no route).

## Позже / интеграции

- [ ] `gw_mqtt`: мост событий наружу (без брокера на устройстве).
- [ ] Backup/restore (экспорт JSON authoring, без `.gwar` — оно пересобирается на устройстве).
