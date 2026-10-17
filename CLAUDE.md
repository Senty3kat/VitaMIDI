# VitaMIDI — заметки для Claude

USB-MIDI хост для PS Vita: kernel-плагин `vitamidi.skprx` + тестовое приложение `VitaMIDI Test` (VMID00001).
Автор — Senty3kat (общение на русском, не программист). У автора **Vita 1000 (fat)** и клавиатура
**M-VAVE SMK-25** (своё питание, USB). Питание VBUS автор делает сам (Y-кабель или внутри клавиатуры).
Комментарии в коде — по-русски.

## Устройство

- `kernel/main.c` — плагин. Хост на мультиконнекторе: `ksceUsbServMacSelect(2, 0)` (вкл) / `(2, 1)` (выкл),
  включается только по `vitamidiStart()` от приложения; выход/падение владельца — выключение (ProcEvent).
  Драйвер SceUsbd: probe ищет интерфейс класс 1 / подкласс 3, attach открывает bulk/interrupt IN,
  `SetConfiguration`, чтение в колбэке с перезапуском. Поколение `gen` защищает от старых колбэков.
- `common/umidi.c` — разбор пакетов USB-MIDI и SPSC-очередь (писатель — колбэк, читатель — syscall).
  Тестируется на хосте: `make -C tests`.
- `include/vitamidi.h` — публичный API (syscall-библиотека `VitaMidi`, NID 0x7A3C000x). Менять структуры —
  только с повышением `VITAMIDI_API_VERSION`.
- `app/main.c` — тест: 5 шагов диагностики, журнал, клавиатура C2–C7, треугольный синт 8 голосов,
  отчёт в `ux0:data/vitamidi/report.txt` (КВАДРАТ).

## Как работаем

- Сборка — GitHub Actions (`vitasdk/vitasdk:latest`). Из сессии видно `gh run list -R Senty3kat/vitamidi`,
  но логи Actions отдают 403. Поэтому workflow кладёт `build-plugin.log` / `build-app.log` в ветку `ci-logs`:
  `git fetch -f origin ci-logs:refs/remotes/origin/ci-logs && git show origin/ci-logs:build-app.log`.
  Локально vitasdk/docker нет.
- Грабли сборки: цель плагина — `vitamidi.skprx-self`; `vita_create_self` — макрос, перед self приложения
  сбрасывать `VITA_ELF_CREATE_FLAGS`; в образе vitasdk нет `ksceKernelUnregisterSysEventHandler`.
- Проверка без vitasdk: плагин — `clang --target=armv7a-none-eabi -mthumb -ffreestanding -fsyntax-only
  -I<vita-headers>/include -Iinclude -Icommon kernel/main.c common/umidi.c`; приложение — gcc с заглушкой
  `vita2d.h`. vita-headers клонировать с github.com/vitasdk/vita-headers.
- Доноры: ViXEn (isage/vixen) — драйвер USB-хоста; vita-usb-ether — схема питания и MacSelect;
  psvita-usb-audio-midi — syscall-паттерн (ENTER_SYSCALL, ProcEvent, event flag).
- Эмулятор (Vita3K) kernel-плагины и USB не эмулирует — проверка только на железе автора, по отчёту.

## Открыто

- Не проверено на железе: всё. Ждём отчёт с шагами 1–5.
- MIDI-выход (pipe_out открывается, но не используется), несколько устройств, хабы.
- Подключение к VitaSampler / VitaStrummer — после проверки.
