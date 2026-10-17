#include "midi_core.h"

#include <string.h>

void psvita_usb_midi_ring_init(PsvitaUsbMidiRing *ring)
{
	if (ring) memset(ring, 0, sizeof(*ring));
}

int psvita_usb_midi_ring_push(PsvitaUsbMidiRing *ring,
					 const PsvitaUsbMidiEvent *event)
{
	if (!ring || !event || ring->count >= PSVITA_USB_MIDI_RING_CAPACITY)
		return 0;
	ring->events[ring->write_index] = *event;
	ring->write_index = (ring->write_index + 1u) % PSVITA_USB_MIDI_RING_CAPACITY;
	ring->count++;
	return 1;
}

int psvita_usb_midi_tx_ring_push(PsvitaUsbMidiRing *ring,
	const PsvitaUsbMidiEvent *event)
{
	if (!ring || !event) return 0;
	/* START, CONTINUE, and STOP supersede clock look-ahead from the previous
	 * transport state. Otherwise those future events hold the transport edge
	 * behind them and are emitted as a burst when their deadlines expire. */
	if (event->size == 1u &&
	    (event->data[0] == 0xfau || event->data[0] == 0xfbu ||
	     event->data[0] == 0xfcu))
		psvita_usb_midi_ring_init(ring);
	return psvita_usb_midi_ring_push(ring, event);
}

uint32_t psvita_usb_midi_ring_pop(PsvitaUsbMidiRing *ring,
					 PsvitaUsbMidiEvent *events, uint32_t capacity)
{
	uint32_t count = 0;
	if (!ring || (!events && capacity)) return 0;
	while (count < capacity && ring->count) {
		events[count++] = ring->events[ring->read_index];
		ring->read_index = (ring->read_index + 1u) % PSVITA_USB_MIDI_RING_CAPACITY;
		ring->count--;
	}
	return count;
}

static int realtime_status_supported(uint8_t status)
{
	return status == 0xf8u || status == 0xfau ||
	       status == 0xfbu || status == 0xfcu;
}

/* VitaMIDI: длина канального сообщения по статусу (0 — не канальное). */
static uint32_t channel_message_size(uint8_t status)
{
	switch (status & 0xf0u) {
	case 0x80u: case 0x90u: case 0xa0u: case 0xb0u: case 0xe0u: return 3u;
	case 0xc0u: case 0xd0u: return 2u;
	default: return 0u;
	}
}

int psvita_usb_midi_event_supported(const PsvitaUsbMidiEvent *event)
{
	uint32_t expected;
	if (!event || event->cable != 0) return 0;
	if (event->size == 1u)
		return realtime_status_supported(event->data[0]);
	/* VitaMIDI: канальные сообщения (ноты, CC, pitch bend, program, pressure). */
	expected = channel_message_size(event->data[0]);
	if (!expected || event->size != expected) return 0;
	if (event->data[1] >= 0x80u) return 0;
	if (expected == 3u && event->data[2] >= 0x80u) return 0;
	return 1;
}

PsvitaUsbMidiParseResult psvita_usb_midi_parse_usb_packets(
	const uint8_t *bytes, size_t length, uint64_t timestamp_us,
	PsvitaUsbMidiRing *destination, uint32_t *dropped)
{
	PsvitaUsbMidiParseResult result = {0, 0, 0};
	if (!bytes || !destination) {
		result.malformed = 1;
		return result;
	}
	/*
	 * USB-MIDI 1.0 requires four-byte Event Packets, but a small embedded host
	 * may forward a short system-real-time transfer as bare MIDI bytes. Keep
	 * this compatibility path limited to transfers too short to contain even
	 * one valid Event Packet, and only accept the four real-time statuses DS-8
	 * supports. The malformed count preserves evidence that the host did this.
	 */
	if (length > 0u && length < 4u) {
		result.malformed = 1;
		for (size_t offset = 0; offset < length; ++offset) {
			PsvitaUsbMidiEvent event;
			memset(&event, 0, sizeof(event));
			event.timestamp_us = timestamp_us;
			event.size = 1;
			event.data[0] = bytes[offset];
			if (!realtime_status_supported(event.data[0])) {
				result.filtered++;
				continue;
			}
			if (!psvita_usb_midi_ring_push(destination, &event)) {
				if (dropped) (*dropped)++;
				continue;
			}
			result.accepted++;
		}
		return result;
	}
	if ((length & 3u) != 0) result.malformed++;
	for (size_t offset = 0; offset + 3u < length; offset += 4u) {
		uint8_t header = bytes[offset];
		uint8_t cable = header >> 4;
		uint8_t cin = header & 0x0fu;
		PsvitaUsbMidiEvent event;
		memset(&event, 0, sizeof(event));
		event.timestamp_us = timestamp_us;
		/*
		 * System real-time messages are global, not channel/cable scoped. Some
		 * embedded USB hosts have been observed to preserve a non-zero virtual
		 * cable nibble or stale CIN/padding bytes even though byte 1 contains a
		 * valid one-byte real-time message. Accept that harmless framing variance
		 * and normalize it to DS-8's single cable.
		 */
		event.cable = 0;
		event.size = 1;
		event.data[0] = bytes[offset + 1u];
		if (!realtime_status_supported(event.data[0])) {
			/* VitaMIDI: канальные сообщения на кабеле 0, CIN совпадает со статусом. */
			uint8_t status = bytes[offset + 1u];
			uint32_t size = channel_message_size(status);
			if (!size || cable != 0u || cin != (status >> 4) ||
			    bytes[offset + 2u] >= 0x80u ||
			    (size == 3u && bytes[offset + 3u] >= 0x80u)) {
				result.filtered++;
				continue;
			}
			event.size = (uint8_t)size;
			event.data[1] = bytes[offset + 2u];
			event.data[2] = size == 3u ? bytes[offset + 3u] : 0u;
		}
		if (!psvita_usb_midi_ring_push(destination, &event)) {
			if (dropped) (*dropped)++;
			continue;
		}
		result.accepted++;
	}
	return result;
}

int psvita_usb_midi_encode_event(const PsvitaUsbMidiEvent *event,
	uint8_t packet[4])
{
	if (!packet || !psvita_usb_midi_event_supported(event)) return 0;
	if (event->size == 1u) {
		packet[0] = 0x0fu;
		packet[1] = event->data[0];
		packet[2] = 0;
		packet[3] = 0;
	} else {
		/* VitaMIDI: CIN = старшая тетрада статуса, кабель 0. */
		packet[0] = (uint8_t)(event->data[0] >> 4);
		packet[1] = event->data[0];
		packet[2] = event->data[1];
		packet[3] = event->size == 3u ? event->data[2] : 0u;
	}
	return 1;
}

uint32_t psvita_usb_midi_encode_due(PsvitaUsbMidiRing *ring, uint64_t now_us,
	uint8_t *bytes, uint32_t capacity)
{
	uint32_t length = 0;
	if (!ring || !bytes) return 0;
	/* VitaMIDI: забираем из очереди все события, срок которых наступил, и
	 * пакуем в одну передачу — аккорд уходит одним пакетом, без растяжки. */
	while (ring->count && length + 4u <= capacity) {
		const PsvitaUsbMidiEvent *next = &ring->events[ring->read_index];
		if (next->timestamp_us && next->timestamp_us > now_us) break;
		if (psvita_usb_midi_encode_event(next, bytes + length)) length += 4u;
		ring->read_index = (ring->read_index + 1u) % PSVITA_USB_MIDI_RING_CAPACITY;
		ring->count--;
	}
	return length;
}
