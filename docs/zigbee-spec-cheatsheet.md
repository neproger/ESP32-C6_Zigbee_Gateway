# ZigBee (05-3474-21) — памятка для gateway проекта

Источник: `docs/docs-05-3474-21-0csg-zigbee-specification.pdf` (ZigBee Specification, rev 21, 2015).

Важно: это **спецификация ZigBee stack / ZDP / ZDO / APS / NWK primitives**, а **ZCL (кластера On/Off, Level Control, атрибуты сенсоров)** описаны в отдельной спецификации *Zigbee Cluster Library (ZCL)*.

---

## 1) Типы устройств (роли в сети)

На уровне ZigBee “тип устройства” чаще всего = **логическая роль**:
- **ZC (Coordinator)** — формирует сеть/Trust Center, обычно `short_addr=0x0000`.
- **ZR (Router)** — маршрутизирует, может принимать join (если permit join).
- **ZED (End Device)** — конечное устройство (часто батарейное). Может быть “sleepy”, тогда часть broadcast’ов/сообщений может пропускать.

Практический вывод для нас:
- `Device_annce`/discovery удобны для “первого знакомства”, но батарейные ZED могут быть не всегда “слушающими”.

---

## 2) Адресация в APS (куда “уходит команда”)

Это ключ к пониманию “свитч шлёт toggle куда?”.

### APSDE-DATA.request: DstAddrMode
Смысловые режимы (см. таблицу параметров APSDE-DATA.request):
- `0x00` — **DstAddress и DstEndpoint не присутствуют** (обычно “через APS binding table”).
- `0x01` — **16-bit group address** (DstEndpoint не задан).
- `0x02` — **16-bit short + endpoint** (unicast на конкретный `short+ep`).
- `0x03` — **64-bit IEEE + endpoint** (unicast на IEEE+ep).

Практический вывод:
- Если у свитча лог “`DST_ADDR_ENDP_NOT_PRESENT (uses APS binding table)`”, то команда пойдёт **только туда, куда есть binding**.
- Group addressing (`0x01`) gateway “увидит” только если он сам добавлен в группу (или слушает эту группу).

---

## 3) Binding: что связываем и как

### APSME-BIND.request (структура binding entry)
Ключевые поля:
- `SrcAddr` (IEEE источника), `SrcEndpoint`, `ClusterId`
- `DstAddrMode`:
  - `0x01` — binding на **group address**
  - `0x03` — binding на **IEEE + endpoint**
- `DstAddr`, `DstEndpoint` (если `DstAddrMode=0x03`)

Практический вывод:
- “кнопка → реле/лампа” обычно делается binding’ом по кластеру `On/Off` (ZCL), и тогда отправка из кнопки в режиме `DstAddrMode=0x00` реально уходит на target из binding table.

---

## 4) ZDP (ZigBee Device Profile): полезные команды (ClusterID)

ZDP-команды идут через APS на **endpoint 0** и состоят из:
- `Transaction sequence number` (TSN, 1 байт)
- `Transaction data` (переменная часть)

Ниже — команды, которые напрямую полезны gateway’ю для discovery/управления сетью:

### Discovery / описание устройств
- `IEEE_addr_req` — `ClusterID = 0x0001`  
  По `NWKAddrOfInterest` получаем IEEE (и иногда список ассоциированных ZED для координатора/роутера).
- `Node_Desc_req` — `ClusterID = 0x0002`  
  Узнать характеристики ноды.
- `Active_EP_req` — (в этой спецификации описан рядом с discovery командами)  
  Получить список активных endpoint’ов.
- `Simple_Desc_req` — получить Simple Descriptor по endpoint’у (кластера in/out, profile_id, device_id).

### “Устройство появилось” / rejoin
- `Device_annce` — `ClusterID = 0x0013`  
  Формат: `NWKAddr` (2) + `IEEEAddr` (8) + `Capability` (1).  
  Рассылается при join/rejoin и полезна как “триггер знакомства”.

### “Родитель объявил детей” (полезно для stability)
- `Parent_annce` — `ClusterID = 0x001F`  
  Используется роутерами/координатором, чтобы объявить известных end devices (конфликты родителей, reboot recovery).

---

## 5) Network management (Mgmt_*): то, что нужно для админки

### Permit join
- `Mgmt_Permit_Joining_req` — `ClusterID = 0x0036`  
  Параметры: `PermitDuration` (0..0xFE), `TC_Significance` (по факту трактуем как 1).  
  В rev21 `0xFF` больше не “forever”, трактуется как `0xFE`; чтобы держать сеть открытой дольше — **переотправлять**.

### Leave (kick)
- `Mgmt_Leave_req` — `ClusterID = 0x0034`  
  Параметры:  
  - `DeviceAddress` (IEEE) — кого просим уйти (0…0 допускается как “self”, зависит от контекста)  
  - `RemoveChildren` (bit) — просить ли “удалить детей”  
  - `Rejoin` (bit) — просить ли сразу rejoin

Практический вывод:
- Чтобы устройство “поняло, что его кикнули”, leave должен **дойти до устройства**, иначе оно продолжит rejoin по старым ключам.
- Если девайс оффлайн/нет маршрута — leave не доставится, и правильный способ “запретить возвращаться” — банлист/повтор leave при `Device_annce`.

### Binding table readback (диагностика “куда шлёт”)
- `Mgmt_Bind_req` — `ClusterID = 0x0033`  
  Параметр `StartIndex`, в ответ приходит binding table.

---

## 6) Как использовать это в нашем “нормализованном event bus”

Рекомендуемая привязка к нашим типам событий:
- `device.join` — при `Device_annce` (`0x0013`): payload `{ cap, ieee, short }`
- `device.leave` — при ответе на leave (Mgmt_Leave_rsp / NLME leave confirm): payload `{ status, rejoin }`
- `zigbee.mgmt.permit_join` — при `Mgmt_Permit_Joining_req`: payload `{ seconds }`
- `zigbee.mgmt.bind_table` — при `Mgmt_Bind_rsp`: payload `{ total, entries:[...] }`
- `zigbee.discovery.*` — при `Active_EP`/`Simple_Desc`/`Node_Desc` ответах: payload структурированный.

Отдельно:
- `zigbee.command` и `zigbee.attr_report` — это уже ZCL-уровень (On/Off/Level, Temp/Humidity и т.п.).

