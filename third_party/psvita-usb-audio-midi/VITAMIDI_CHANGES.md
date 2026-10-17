# Изменения VitaMIDI относительно оригинала

Оригинал: [intermynd-instruments/psvita-usb-audio-midi](https://github.com/intermynd-instruments/psvita-usb-audio-midi)
v0.2 (коммит `3ef1f53`), лицензия MIT. Все правки помечены комментарием `VitaMIDI`.

Оригинальный плагин сделан для DS-8 и пропускает только MIDI-клок, Start, Continue и Stop.
Здесь он расширен, чтобы Vita работала как MIDI-клавиатура/контроллер для Ableton и других DAW.

1. **Канальные сообщения в обе стороны** (`src/common/midi_core.c`): Note On/Off, Control Change,
   Pitch Bend, Program Change, Channel Pressure, Poly Aftertouch. Принимаются только корректные
   пакеты: кабель 0, CIN совпадает со статусом, байты данных < 0x80. SysEx, Song Position и
   прочее по-прежнему отфильтрованы. Совместимость приёма клока с «неаккуратными» хостами сохранена.
2. **Пачки при отправке** (`psvita_usb_midi_encode_due`, `process_tx` в `src/kernel/kernel.c`):
   все события, срок которых наступил, уходят одной USB-передачей (до 15 пакетов, 60 байт).
   Аккорд не растягивается на несколько миллисекунд. Логика сроков для клока DS-8 не изменена.
3. **Тесты** (`tests/core_tests.c`): обновлён тест фильтра, добавлены
   `test_vitamidi_channel_messages` и `test_vitamidi_encode_due_batches`.

Версия API (`0x00030000`) и формат структур не менялись: DS-8 v0.7 работает с этой сборкой.
