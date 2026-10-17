/* Разбор пакетов USB-MIDI 1.0 и очередь событий. Без зависимостей от Vita. */
#ifndef UMIDI_H
#define UMIDI_H

#include <stdint.h>
#include "vitamidi.h"

/* Сколько байт MIDI несёт пакет с данным кодом CIN (0 — пакет пропускаем). */
int umidi_cin_size(uint8_t cin);

/* Очередь событий: один писатель (USB-колбэк) и один читатель (системный вызов). */
#define UMIDI_RING_SIZE 512 /* степень двойки */

typedef struct UmidiRing {
	volatile uint32_t head; /* пишет только писатель */
	volatile uint32_t tail; /* пишет только читатель */
	VitaMidiEvent ev[UMIDI_RING_SIZE];
} UmidiRing;

void umidi_ring_reset(UmidiRing *r);
int umidi_ring_push(UmidiRing *r, const VitaMidiEvent *e);       /* 0 — нет места */
uint32_t umidi_ring_pop(UmidiRing *r, VitaMidiEvent *out, uint32_t max);

typedef struct UmidiStats {
	uint32_t packets;
	uint32_t events;
	uint32_t dropped;
} UmidiStats;

/* Разобрать буфер из bulk-передачи (длина кратна 4, хвост отбрасывается). */
void umidi_parse(const uint8_t *buf, uint32_t len, uint64_t timestamp_us,
	UmidiRing *ring, UmidiStats *st);

#endif
