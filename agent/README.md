# deskos agent (Windows)

ROADMAP №15. Устройство-клиент подключается к нему по протоколу из
[docs/host-protocol.md](../docs/host-protocol.md) — mDNS для обнаружения,
WebSocket для транспорта. Реализует сторону агента: `.NET 10`, WinForms
(только трей-иконка, без окна).

## Запуск

```
cd agent\DeskosAgent
dotnet run
```

При первом запуске генерируется токен и кладётся в
`%LOCALAPPDATA%\DeskosAgent\config.json`. Значок в трее → **Copy pairing
token** → вставить в `/sd/agent.json` на карте устройства:

```json
{"token":"<скопированный токен>"}
```

Устройство перечитывает этот файл каждые 3с само, перезагрузка не нужна.

## Публикация одним файлом

```
dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

Даёт один `.exe` без установленного .NET Runtime на целевой машине.

## Что уже есть (v1)

- Обнаружение — `_deskos-agent._tcp.local.` (`Makaretu.Dns.Multicast`).
- Транспорт — ручной upgrade WebSocket поверх `TcpListener`
  (`System.Net.WebSockets.WebSocket.CreateFromStream`), **не**
  `HttpListener` — тот требует admin-прав или `netsh http add urlacl`
  для прослушивания wildcard-адреса, а обычный трей-агент не должен
  просить пользователя об этом при первом запуске.
- Один топик — `cpu` (`{"percent":N,"temp_c":null}`), обновляется раз в 2с.
  CPU% — через `GetSystemTimes` (P/Invoke), **не**
  `System.Diagnostics.PerformanceCounter`: тот зависит от реестра счётчиков
  производительности (PerfLib), который на части машин повреждён —
  поймано именно на машине, где это писалось.
- Трей: **Copy pairing token**, **Open config folder**, **Start with
  Windows** (реестр `HKCU\...\Run`, без установщика), **Exit**.

## Чего пока нет

- **Температура** — `temp_c` всегда `null`. Надёжный путь на Windows —
  `LibreHardwareMonitorLib`, но это kernel-драйвер и admin-права, не для
  первой версии простого трей-агента. Формат сообщения это уже
  предусматривает (`temp_c: number | null`), так что добавление позже —
  не изменение протокола.
- Темы `now_playing`, `build_status` из ROADMAP №14 — не реализованы.
- Установщик/автообновление — только "один .exe + реестр автозагрузки".

Проверено на живой плате 19.08.2026: устройство нашло агента через mDNS,
прошло hello/hello_ack, подписалось на `cpu` из тестового `ui.jsonl` и
показывало реальную (не тестовую) загрузку процессора этого ПК в реальном
времени.
