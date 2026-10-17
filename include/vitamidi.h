/*
 * VitaMIDI — USB-MIDI хост для PS Vita (kernel-плагин vitamidi.skprx).
 *
 * Публичный API для приложений. Приложение линкует libVitaMidi_stub_weak.a
 * и вызывает функции ниже (это системные вызовы плагина).
 *
 *   if (vitamidiGetVersion() != VITAMIDI_API_VERSION) -> плагин не установлен
 *   vitamidiStart();          включить режим USB-хоста (мультиконнектор Vita 1000)
 *   vitamidiRead(ev, 64, 0);  забрать накопившиеся MIDI-события
 *   vitamidiStop();           вернуть порт в обычный режим (MTP / VitaShell USB)
 *
 * Пока режим хоста включён, Vita не видна компьютеру по USB-кабелю.
 * Если приложение закрылось или упало, плагин сам выключает хост.
 */
#ifndef VITAMIDI_H
#define VITAMIDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITAMIDI_API_VERSION 1

/* Коды ошибок (отрицательные). */
#define VITAMIDI_ERROR_BUSY      ((int)0x80F70001) /* хост занят другим приложением */
#define VITAMIDI_ERROR_NOT_OWNER ((int)0x80F70002) /* вызов не от владельца */
#define VITAMIDI_ERROR_PARAM     ((int)0x80F70003)
#define VITAMIDI_ERROR_HOST      ((int)0x80F70004) /* не удалось включить режим хоста */
#define VITAMIDI_ERROR_MEMORY    ((int)0x80F70005)

/* Одно MIDI-событие. Для нот: data[0] = 0x9n/0x8n, data[1] = нота, data[2] = сила. */
typedef struct VitaMidiEvent {
	uint64_t timestamp_us; /* системное время прихода пакета, мкс */
	uint8_t cable;         /* виртуальный кабель USB-MIDI, 0..15 */
	uint8_t size;          /* сколько байт в data, 1..3 */
	uint8_t data[3];
	uint8_t reserved[3];
} VitaMidiEvent;

/* Флаги состояния. */
#define VITAMIDI_STATE_HOST_ON   (1u << 0) /* режим хоста включён */
#define VITAMIDI_STATE_ATTACHED  (1u << 1) /* MIDI-устройство подключено и читается */
#define VITAMIDI_STATE_STALLED   (1u << 2) /* чтение остановлено из-за ошибок */
#define VITAMIDI_STATE_PSTV      (1u << 3) /* PS TV: порты хостовые сами по себе */

typedef struct VitaMidiStatus {
	uint32_t size;          /* заполнить sizeof(VitaMidiStatus) перед вызовом */
	uint32_t api_version;
	uint32_t state;         /* VITAMIDI_STATE_* */
	int32_t  host_result;   /* результат последнего включения/выключения хоста */

	/* Любое устройство, которое Vita увидела на шине (даже не MIDI) —
	 * так видно, что питание и кабель в порядке. */
	uint32_t devices_seen;
	uint16_t seen_vid;
	uint16_t seen_pid;
	uint8_t  seen_class;    /* класс интерфейса, на котором остановился поиск */
	uint8_t  seen_is_midi;
	uint8_t  reserved0[2];

	/* Подключённое MIDI-устройство. */
	uint16_t vid;
	uint16_t pid;
	uint8_t  ep_in;
	uint8_t  ep_out;
	uint16_t max_packet;

	/* Счётчики. */
	uint32_t transfers;     /* завершённых чтений */
	uint32_t packets;       /* 4-байтовых пакетов USB-MIDI */
	uint32_t events;        /* событий положено в очередь */
	uint32_t dropped;       /* потеряно из-за переполнения очереди */
	uint32_t errors;        /* ошибок передачи */
	int32_t  last_error;
	int32_t  attach_result; /* код последней неудачной попытки подключения */
} VitaMidiStatus;

/* Системные вызовы плагина. */
int vitamidiGetVersion(void);
int vitamidiStart(void);
int vitamidiStop(void);
int vitamidiGetStatus(VitaMidiStatus *status);
/* Возвращает число событий (0..max). timeout_us > 0 — ждать первое событие
 * не дольше указанного (не больше 1 000 000). */
int vitamidiRead(VitaMidiEvent *events, uint32_t max, uint32_t timeout_us);

#define VITAMIDI_MAX_READ 64

#ifdef __cplusplus
}
#endif

#endif
