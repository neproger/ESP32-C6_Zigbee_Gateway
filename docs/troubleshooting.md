# Troubleshooting

См. также: `docs/getting-started.md`, `docs/current-state.md`.

## Web UI не открывается по ссылке `http://<ip>/`

Проверь, что ПК и ESP32 в одной сети/подсети.

1) На ПК выполни `ping <ip>` (например `ping 192.168.1.105`).
2) Если ping не проходит:
   - часто включён `AP/client isolation` (особенно на guest Wi‑Fi),
   - либо ПК подключён к другой сети/VPN.
3) Если ping проходит, но браузер не открывает:
   - попробуй `curl http://<ip>/` или открой с телефона в той же Wi‑Fi.

## Wi‑Fi: “No known SSIDs found in scan results”

Обычно причина:
- в `main/wifi_aps_config.h` оставлены заглушки (`CHANGE_ME_SSID`),
- рядом нет нужной 2.4 GHz сети,
- SSID скрыт (scan может не увидеть),
- точка доступна только на 5 GHz.

## VS Code: `ENOENT ... esptool.py` / пути на `C:\\Users\\ASUS\\...`

Если проект/настройки были скопированы с другого ПК, проверь `.vscode/settings.json`:

- `idf.espIdfPathWin`
- `idf.toolsPathWin`
- `idf.pythonInstallPath`

И убедись, что они указывают на твою установку ESP‑IDF/инструментов.

## PowerShell: `export.ps1 cannot be loaded because running scripts is disabled`

Это политика выполнения PowerShell. Варианты:
- использовать ESP‑IDF Terminal в VS Code,
- или запускать сборку через `cmd.exe` (`export.bat`),
- или временно запустить PowerShell с `-ExecutionPolicy Bypass`.

## Guru Meditation / Stack protection fault в `httpd`

Причина обычно — большие буферы на стеке в обработчиках HTTP.
Решение: стримить ответ chunk’ами (`httpd_resp_sendstr_chunk`) или выделять память в heap.
