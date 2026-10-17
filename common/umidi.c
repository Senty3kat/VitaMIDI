#include "umidi.h"

int umidi_cin_size(uint8_t cin)
{
	switch (cin & 0x0f) {
	case 0x0: case 0x1: return 0;      /* зарезервировано */
	case 0x2: return 2;                /* системное общее, 2 байта */
	case 0x3: return 3;                /* системное общее, 3 байта */
	case 0x4: return 3;                /* SysEx начинается/продолжается */
	case 0x5: return 1;                /* SysEx кончается 1 байтом / 1 байт системного */
	case 0x6: return 2;                /* SysEx кончается 2 байтами */
	case 0x7: return 3;                /* SysEx кончается 3 байтами */
	case 0xC: case 0xD: return 2;      /* Program Change, Channel Pressure */
	case 0xF: return 1;                /* одиночный байт (клок и т.п.) */
	default: return 3;                 /* 0x8..0xB, 0xE: ноты, CC, Pitch Bend */
	}
}

void umidi_ring_reset(UmidiRing *r)
{
	r->head = 0;
	r->tail = 0;
}

int umidi_ring_push(UmidiRing *r, const VitaMidiEvent *e)
{
	uint32_t head = r->head;
	uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
	if (head - tail >= UMIDI_RING_SIZE)
		return 0;
	r->ev[head & (UMIDI_RING_SIZE - 1)] = *e;
	__atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
	return 1;
}

uint32_t umidi_ring_pop(UmidiRing *r, VitaMidiEvent *out, uint32_t max)
{
	uint32_t tail = r->tail;
	uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
	uint32_t n = 0;
	while (tail != head && n < max) {
		out[n++] = r->ev[tail & (UMIDI_RING_SIZE - 1)];
		tail++;
	}
	__atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
	return n;
}

void umidi_parse(const uint8_t *buf, uint32_t len, uint64_t timestamp_us,
	UmidiRing *ring, UmidiStats *st)
{
	uint32_t off;
	for (off = 0; off + 4 <= len; off += 4) {
		const uint8_t *p = buf + off;
		VitaMidiEvent e;
		int size;
		if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0)
			continue; /* заполнение в конце передачи */
		st->packets++;
		size = umidi_cin_size(p[0]);
		if (size == 0)
			continue;
		e.timestamp_us = timestamp_us;
		e.cable = p[0] >> 4;
		e.size = (uint8_t)size;
		e.data[0] = p[1];
		e.data[1] = size > 1 ? p[2] : 0;
		e.data[2] = size > 2 ? p[3] : 0;
		e.reserved[0] = e.reserved[1] = e.reserved[2] = 0;
		if (umidi_ring_push(ring, &e))
			st->events++;
		else
			st->dropped++;
	}
}
