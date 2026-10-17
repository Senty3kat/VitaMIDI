/* Тесты разбора USB-MIDI на компьютере: make -C tests */
#include <stdio.h>
#include <string.h>
#include "umidi.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static UmidiRing ring;

int main(void)
{
	UmidiStats st;
	VitaMidiEvent ev[8];
	uint32_t n, i;

	/* Note On C4 vel 100, Note Off C4, CC1=64, Pitch Bend, клок, заполнение нулями */
	uint8_t buf[] = {
		0x09, 0x90, 60, 100,
		0x08, 0x80, 60, 0,
		0x0B, 0xB0, 1, 64,
		0x0E, 0xE0, 0x00, 0x40,
		0x0F, 0xF8, 0, 0,
		0x1C, 0xC1, 5, 0,     /* Program Change на кабеле 1 */
		0, 0, 0, 0,
		0x00, 0x11, 0x22, 0x33, /* CIN 0 — пропуск */
		0x09, 0x90             /* обрывок пакета — отбросить */
	};

	umidi_ring_reset(&ring);
	memset(&st, 0, sizeof(st));
	umidi_parse(buf, sizeof(buf), 1234, &ring, &st);
	CHECK(st.packets == 7);
	CHECK(st.events == 6);
	CHECK(st.dropped == 0);

	n = umidi_ring_pop(&ring, ev, 8);
	CHECK(n == 6);
	CHECK(ev[0].size == 3 && ev[0].data[0] == 0x90 && ev[0].data[1] == 60 && ev[0].data[2] == 100);
	CHECK(ev[0].timestamp_us == 1234 && ev[0].cable == 0);
	CHECK(ev[1].data[0] == 0x80);
	CHECK(ev[2].data[0] == 0xB0 && ev[2].data[1] == 1 && ev[2].data[2] == 64);
	CHECK(ev[3].data[0] == 0xE0 && ev[3].data[2] == 0x40);
	CHECK(ev[4].size == 1 && ev[4].data[0] == 0xF8 && ev[4].data[1] == 0);
	CHECK(ev[5].size == 2 && ev[5].cable == 1 && ev[5].data[1] == 5 && ev[5].data[2] == 0);
	CHECK(umidi_ring_pop(&ring, ev, 8) == 0);

	/* Переполнение очереди и перенос индексов через край. */
	memset(&st, 0, sizeof(st));
	for (i = 0; i < UMIDI_RING_SIZE + 10; i++)
		umidi_parse(buf, 4, i, &ring, &st);
	CHECK(st.events == UMIDI_RING_SIZE);
	CHECK(st.dropped == 10);
	n = 0;
	while ((i = umidi_ring_pop(&ring, ev, 8)) > 0) {
		if (n == 0) CHECK(ev[0].timestamp_us == 0);
		n += i;
	}
	CHECK(n == UMIDI_RING_SIZE);
	ring.head = ring.tail = 0xFFFFFFF0u;
	memset(&st, 0, sizeof(st));
	for (i = 0; i < 40; i++)
		umidi_parse(buf, 4, i, &ring, &st);
	n = 0;
	while ((i = umidi_ring_pop(&ring, ev, 8)) > 0)
		n += i;
	CHECK(n == 40);

	printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
	return fails != 0;
}
