# VitaMIDI

MIDI для PS Vita в обе стороны:

- **Vita → компьютер:** Vita как USB-MIDI клавиатура для Ableton и других DAW (приложение **VitaMIDI Controller**).
- **MIDI-клавиатура → Vita:** kernel-плагин `vitamidi.skprx` делает Vita USB-хостом для
  class-compliant MIDI-клавиатур, например M-VAVE SMK-25 (приложение **VitaMIDI Test**).

> Экспериментальный kernel-код. Ставь на свой риск. Если Vita не загружается —
> держи **L** при включении (плагины не грузятся), убери строку из `config.txt`.

## Режим 1: Vita → Ableton (Vita как MIDI-клавиатура)

Vita подключается к компьютеру обычным USB-кабелем и видна как MIDI-устройство
**PS Vita USB** — без драйверов, на Windows, macOS и Linux. Плагин —
`psvita_usb_audio_midi.skprx` из [DS-8](https://github.com/intermynd-instruments/psvita-usb-audio-midi),
расширенный в VitaMIDI: в оригинале он передаёт только клок, здесь — ноты, CC,
pitch bend и остальные канальные сообщения в обе стороны
(подробности — `third_party/psvita-usb-audio-midi/VITAMIDI_CHANGES.md`). DS-8 с ним тоже работает.

### Установка

1. Скачай артефакт `vitamidi` из последней сборки (вкладка **Actions**).
2. `psvita_usb_audio_midi.skprx` положи в `ur0:tai/` и добавь под `*KERNEL` в `ur0:tai/config.txt`:
   ```
   ur0:tai/psvita_usb_audio_midi.skprx
   ```
   Если раньше стоял оригинальный плагин DS-8 с тем же именем — просто замени файл.
3. Перезагрузи Vita, установи `vitamidi_controller.vpk`.
4. В VitaShell выключи USB-режим. Плагины `vitastick` и `udcd_uvc` должны быть выключены.

### Ableton

1. Запусти **VitaMIDI Controller**, подключи кабель. Наверху должно загореться
   `CONNECTED to computer`.
2. Ableton → **Preferences → Link, Tempo & MIDI**. В списке портов у **PS Vita USB**:
   - вход (**In**): включи **Track** (и **Remote**, если хочешь назначать ручки через MIDI Map);
   - выход (**Out**): включи **Track**, если хочешь, чтобы ноты из Ableton светились на Vita.
3. Создай MIDI-дорожку с инструментом, включи мониторинг или запись — играй на экране Vita.

### Управление контроллером

| Что | Действие |
| --- | --- |
| Сенсорный экран | клавиши, до 6 пальцев; сила — по высоте нажатия (ниже — громче); палец можно вести по клавишам |
| L / R | октава вниз / вверх |
| Левый стик ←→ | pitch bend |
| Правый стик ↑ | модуляция (CC1) |
| Задняя панель | CC74 (влево-вправо), CC71 (вверх-вниз) — удобно назначить на фильтр |
| Треугольник | sustain (CC64), пока держишь |
| SELECT | MIDI-канал 1–16 |
| Круг | звук на самой Vita вкл/выкл |
| X | переподключить USB |
| Квадрат | отчёт в `ux0:data/vitamidi/controller_report.txt` |

Оранжевые клавиши — сыгранные на Vita, голубые — пришедшие из Ableton.

## Режим 2: MIDI-клавиатура → Vita (USB-хост)

### Что нужно

- Vita 1000 (мультиконнектор) с HENkaku/Ensō и taiHEN. PS TV тоже должен работать
  (у него USB-порты хостовые), Vita 2000 пока не проверялась.
- Стандартный USB-кабель Vita.
- **Питание 5 В на линию VBUS.** В режиме хоста Vita питание в кабель не подаёт.
  Варианты: Y-кабель, переходник USB-A мама–мама с подачей 5 В от зарядки,
  USB-хаб с внешним блоком питания, или питание внутри клавиатуры на её USB-разъём.
  Схема Y-кабеля: <https://github.com/isage/vita-usb-ether#hardware>

```
                 + 5 В от зарядки
                 |
  кабель Vita  VBUS ----+---- VBUS  клавиатура
               D-   ---------- D-
               D+   ---------- D+
               GND  ----+---- GND
                 |
                 - земля зарядки
```

### Установка

1. Скачай артефакт `vitamidi` из последней сборки GitHub Actions
   (вкладка **Actions** → верхний зелёный запуск → **Artifacts**).
2. `vitamidi.skprx` скопируй в `ur0:tai/`.
3. В `ur0:tai/config.txt` под строку `*KERNEL` добавь:
   ```
   ur0:tai/vitamidi.skprx
   ```
4. Перезагрузи Vita.
5. Установи `vitamidi_test.vpk` через VitaShell.

Плагин ничего не делает, пока его не попросит приложение: обычный USB
(VitaShell USB, Content Manager) работает как раньше.

### Проверка

Запусти **VitaMIDI Test**. На экране 5 шагов — зелёный квадрат значит «есть»:

1. **Plugin loaded** — плагин установлен.
2. **USB host mode** — порт переключён в режим хоста (приложение делает это само,
   X — вкл/выкл). Пока хост включён, Vita не видна компьютеру по кабелю.
3. **Device on the bus** — Vita увидела хоть какое-то USB-устройство. Если тут
   пусто — нет питания на VBUS или плохой контакт.
4. **MIDI keyboard attached** — устройство распознано как MIDI и открыто.
5. **Notes arriving** — ноты идут. Нажатые клавиши светятся оранжевым и звучат
   треугольной волной; `lat` — сколько микросекунд прошло от прихода пакета до звука.

**Квадрат** сохраняет отчёт в `ux0:data/vitamidi/report.txt` — пришли его, если
что-то не работает.

## Для приложений

API — `include/vitamidi.h`. Приложение линкует `libVitaMidi_stub_weak.a`
(получается при сборке) и работает без плагина, если его нет:

```c
#include "vitamidi.h"

if (vitamidiGetVersion() == VITAMIDI_API_VERSION) {
    vitamidiStart();                      /* включить хост */
    VitaMidiEvent ev[64];
    int n = vitamidiRead(ev, 64, 0);      /* забрать события, не ждать */
    /* ev[i].data[0] = 0x90 нота вкл, 0x80 выкл; data[1] нота; data[2] сила */
    vitamidiStop();                       /* перед выходом */
}
```

Если приложение закрылось или упало, плагин сам выключает хост.

## Как это устроено

- Режим хоста на мультиконнекторе — `ksceUsbServMacSelect(2, 0)`, как в
  [ViXEn](https://github.com/isage/vixen) и [vita-usb-ether](https://github.com/isage/vita-usb-ether).
- Плагин регистрирует USB-драйвер (`SceUsbd`) для интерфейсов Audio / MIDIStreaming
  (класс 1, подкласс 3), читает bulk-конечную точку и разбирает 4-байтовые пакеты
  USB-MIDI 1.0 в очередь на 512 событий.
- Системный драйвер `usb_charge` снимается при первом включении хоста (как в ViXEn),
  иначе он перехватывает часть устройств.

## Сборка

Собирается в GitHub Actions (контейнер `vitasdk/vitasdk`). Локально:

```sh
cmake -S . -B build && cmake --build build
make -C tests          # тесты разбора USB-MIDI на компьютере
```

## Лицензия

MIT. Идеи и приёмы работы с USB-хостом — из ViXEn и vita-usb-ether (isage, MIT).
