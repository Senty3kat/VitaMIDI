/*
 * VitaMIDI Controller — Vita как USB-MIDI клавиатура для Ableton и других DAW.
 *
 * Работает через плагин psvita_usb_audio_midi.skprx (сборка VitaMIDI с полным MIDI).
 * Компьютер видит устройство «PS Vita USB».
 *
 * Управление:
 *   сенсорный экран  — клавиши (до 6 пальцев, сила — по высоте нажатия на клавише)
 *   L / R            — октава вниз / вверх
 *   левый стик ←→    — pitch bend
 *   правый стик ↑    — модуляция (CC1)
 *   задняя панель    — CC74 (по горизонтали) и CC71 (по вертикали)
 *   ТРЕУГОЛЬНИК      — sustain (CC64), пока держишь
 *   SELECT           — MIDI-канал 1..16
 *   КРУГ             — звук на самой Vita вкл/выкл
 *   X                — переподключить USB
 *   КВАДРАТ          — сохранить отчёт
 *   START            — выход
 */
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "psvita_usb_audio_midi.h"

#define APP_VERSION "0.1"
#define SR 48000
#define BLOCK 256
#define VOICES 8

/* ---------- раскладка экрана 960x544 ---------- */
#define KEYS_Y 300
#define KEYS_H 236
#define WHITE_KEYS 15             /* две октавы и верхняя до */
#define KEY_W 64
#define BLACK_H 140
#define MAX_TOUCH 8

/* ---------- цвета ---------- */
#define C_BG     RGBA8(0x1e, 0x1b, 0x18, 0xff)
#define C_TEXT   RGBA8(0xf2, 0xe8, 0xd5, 0xff)
#define C_DIM    RGBA8(0x8c, 0x82, 0x72, 0xff)
#define C_OK     RGBA8(0x8f, 0xd6, 0x7a, 0xff)
#define C_WARN   RGBA8(0xf2, 0xb2, 0x4b, 0xff)
#define C_BAD    RGBA8(0xf0, 0x6a, 0x50, 0xff)
#define C_ORANGE RGBA8(0xf2, 0x8c, 0x28, 0xff)
#define C_BLUE   RGBA8(0x6c, 0xb4, 0xf0, 0xff)
#define C_WHITE  RGBA8(0xf2, 0xe8, 0xd5, 0xff)
#define C_BLACK  RGBA8(0x2a, 0x25, 0x20, 0xff)

/* ---------- состояние ---------- */

static vita2d_pgf *font;
static volatile int app_running = 1;
static volatile int usb_ok;            /* мост загружен и аренда USB получена */
static volatile int synth_on;
static int channel;                    /* 0..15 */
static int octave = 4;                 /* нижняя до = C4 (60) при octave 4 */

static volatile unsigned char own_keys[128];   /* сыграно на Vita */
static volatile unsigned char host_keys[128];  /* пришло с компьютера */

static unsigned sent_count, recv_count, send_errors;
static int last_write_error;

/* Журнал: строки событий для экрана. */
#define LOG_LINES 8
static char log_lines[LOG_LINES][56];
static int log_count;

static void log_add(const char *s)
{
	memmove(log_lines[1], log_lines[0], sizeof(log_lines[0]) * (LOG_LINES - 1));
	snprintf(log_lines[0], sizeof(log_lines[0]), "%s", s);
	if (log_count < LOG_LINES) log_count++;
}

/* ---------- синт (тот же треугольник, что в тесте) ---------- */

typedef struct Voice { int note; float phase, inc, amp, target; unsigned age; } Voice;
static Voice voices[VOICES];
static unsigned voice_clock;
static SceUID synth_lock;

static void synth_note_on(int note, int vel)
{
	int i, best = 0;
	sceKernelLockMutex(synth_lock, 1, NULL);
	for (i = 0; i < VOICES; i++) if (voices[i].note == note) { best = i; goto use; }
	for (i = 0; i < VOICES; i++) if (voices[i].note < 0 && voices[i].amp < 0.0001f) { best = i; goto use; }
	for (i = 1; i < VOICES; i++) if (voices[i].age < voices[best].age) best = i;
use:
	voices[best].note = note;
	voices[best].inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / SR;
	voices[best].target = 0.04f + 0.16f * vel / 127.0f;
	voices[best].age = ++voice_clock;
	sceKernelUnlockMutex(synth_lock, 1);
}

static void synth_note_off(int note)
{
	int i;
	sceKernelLockMutex(synth_lock, 1, NULL);
	for (i = 0; i < VOICES; i++)
		if (voices[i].note == note) { voices[i].note = -1; voices[i].target = 0; }
	sceKernelUnlockMutex(synth_lock, 1);
}

/* ---------- отправка MIDI ---------- */

static PsvitaUsbMidiEvent out_ev[PSVITA_USB_MIDI_MAX_EVENTS];
static unsigned out_n;

static void queue3(uint8_t status, uint8_t d1, uint8_t d2)
{
	PsvitaUsbMidiEvent *e;
	if (out_n >= PSVITA_USB_MIDI_MAX_EVENTS) return;
	e = &out_ev[out_n++];
	memset(e, 0, sizeof(*e));
	e->size = 3;
	e->data[0] = (uint8_t)(status | channel);
	e->data[1] = d1 & 0x7f;
	e->data[2] = d2 & 0x7f;
}

static void flush_out(void)
{
	int r;
	if (!out_n) return;
	if (usb_ok) {
		r = psvitaUsbMidiWrite(out_ev, out_n);
		if (r < 0) { send_errors++; last_write_error = r; }
		else sent_count += (unsigned)r;
		if (r >= 0 && (unsigned)r < out_n) send_errors += out_n - (unsigned)r; /* отфильтровал плагин */
	}
	out_n = 0;
}

static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void play_note(int note, int vel)
{
	char s[56];
	queue3(0x90, (uint8_t)note, (uint8_t)vel);
	own_keys[note] = (unsigned char)vel;
	if (synth_on) synth_note_on(note, vel);
	snprintf(s, sizeof(s), "-> ON  %s%d vel %d", NOTE_NAMES[note % 12], note / 12 - 1, vel);
	log_add(s);
}

static void stop_note(int note)
{
	queue3(0x80, (uint8_t)note, 0);
	own_keys[note] = 0;
	synth_note_off(note);
}

/* ---------- приём MIDI (аудиопоток: минимальная задержка до звука) ---------- */

#define EVQ 256
static PsvitaUsbMidiEvent evq[EVQ];
static volatile unsigned evq_head;
static unsigned evq_tail;

static void handle_in(const PsvitaUsbMidiEvent *e)
{
	uint8_t st = e->data[0] & 0xf0;
	if (e->size == 3 && st == 0x90 && e->data[2]) {
		host_keys[e->data[1]] = e->data[2];
		if (synth_on) synth_note_on(e->data[1], e->data[2]);
	} else if (e->size == 3 && (st == 0x80 || st == 0x90)) {
		host_keys[e->data[1]] = 0;
		if (!own_keys[e->data[1]]) synth_note_off(e->data[1]);
	} else if (e->size == 3 && st == 0xB0 && (e->data[1] == 120 || e->data[1] == 123)) {
		int i;
		for (i = 0; i < 128; i++) { host_keys[i] = 0; if (!own_keys[i]) synth_note_off(i); }
	}
}

static int audio_thread(SceSize args, void *argp)
{
	static int16_t buf[2][BLOCK * 2];
	int which = 0;
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, BLOCK, SR, SCE_AUDIO_OUT_MODE_STEREO);
	(void)args; (void)argp;
	if (port < 0) return 0;
	while (app_running) {
		int i, v;
		if (usb_ok) {
			PsvitaUsbMidiEvent ev[32];
			int n = psvitaUsbMidiRead(ev, 32);
			for (i = 0; i < n; i++) {
				handle_in(&ev[i]);
				evq[evq_head % EVQ] = ev[i];
				evq_head++;
			}
		}
		sceKernelLockMutex(synth_lock, 1, NULL);
		for (i = 0; i < BLOCK; i++) {
			float s = 0;
			for (v = 0; v < VOICES; v++) {
				Voice *o = &voices[v];
				float tri;
				o->amp += (o->target - o->amp) * (o->target > o->amp ? 0.01f : 0.0015f);
				if (o->amp < 0.00001f) continue;
				o->phase += o->inc;
				if (o->phase >= 1.0f) o->phase -= 1.0f;
				tri = o->phase < 0.5f ? 4.0f * o->phase - 1.0f : 3.0f - 4.0f * o->phase;
				s += tri * o->amp;
			}
			if (s > 1) s = 1;
			if (s < -1) s = -1;
			buf[which][i * 2] = buf[which][i * 2 + 1] = (int16_t)(s * 32000);
		}
		sceKernelUnlockMutex(synth_lock, 1);
		sceAudioOutOutput(port, buf[which]);
		which ^= 1;
	}
	sceAudioOutReleasePort(port);
	return 0;
}

/* ---------- клавиатура на экране ---------- */

static const int WHITE_PC[7] = {0, 2, 4, 5, 7, 9, 11};
static const int HAS_BLACK[7] = {1, 1, 0, 1, 1, 1, 0}; /* чёрная справа от белой */

static int base_note(void) { return 12 * (octave + 1); }

/* Нота под точкой экрана, сила по высоте (ниже — громче). -1 — мимо. */
static int key_at(int x, int y, int *vel)
{
	int w, rel;
	if (y < KEYS_Y || y >= KEYS_Y + KEYS_H || x < 0 || x >= WHITE_KEYS * KEY_W) return -1;
	rel = y - KEYS_Y;
	w = x / KEY_W;
	if (rel < BLACK_H) {
		int xi = x - w * KEY_W;
		/* правая треть белой — чёрная справа, левая треть — чёрная слева */
		if (xi > KEY_W * 2 / 3 && w < WHITE_KEYS - 1 && HAS_BLACK[w % 7]) {
			*vel = 40 + 87 * rel / BLACK_H;
			return base_note() + 12 * (w / 7) + WHITE_PC[w % 7] + 1;
		}
		if (xi < KEY_W / 3 && w > 0 && HAS_BLACK[(w - 1) % 7]) {
			*vel = 40 + 87 * rel / BLACK_H;
			return base_note() + 12 * ((w - 1) / 7) + WHITE_PC[(w - 1) % 7] + 1;
		}
	}
	*vel = 40 + 87 * rel / KEYS_H;
	return base_note() + 12 * (w / 7) + WHITE_PC[w % 7];
}

static unsigned key_color(int note, unsigned base)
{
	if (note < 0 || note > 127) return base;
	if (own_keys[note]) return C_ORANGE;
	if (host_keys[note]) return C_BLUE;
	return base;
}

static void draw_keys(void)
{
	int w;
	for (w = 0; w < WHITE_KEYS; w++) {
		int note = base_note() + 12 * (w / 7) + WHITE_PC[w % 7];
		vita2d_draw_rectangle(w * KEY_W + 1, KEYS_Y, KEY_W - 2, KEYS_H, key_color(note, C_WHITE));
		if (WHITE_PC[w % 7] == 0) {
			char s[12];
			snprintf(s, sizeof(s), "C%d", note / 12 - 1);
			vita2d_pgf_draw_text(font, w * KEY_W + 8, KEYS_Y + KEYS_H - 12, C_DIM, 1.0f, s);
		}
	}
	for (w = 0; w < WHITE_KEYS - 1; w++) {
		if (!HAS_BLACK[w % 7]) continue;
		int note = base_note() + 12 * (w / 7) + WHITE_PC[w % 7] + 1;
		vita2d_draw_rectangle(w * KEY_W + KEY_W * 2 / 3, KEYS_Y, KEY_W * 2 / 3, BLACK_H, key_color(note, C_BLACK));
	}
}

/* ---------- USB ---------- */

static int api_version, acquire_result, load_result;

static const char *explain(int r)
{
	switch (r) {
	case PSVITA_USB_AUDIO_MIDI_ERROR_BUSY: return "USB is used by another app (DS-8?) - close it";
	case PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE: return "turn off VitaShell USB mode, replug cable";
	case PSVITA_USB_AUDIO_MIDI_ERROR_CONFLICT: return "disable vitastick / udcd_uvc plugins";
	case PSVITA_USB_AUDIO_MIDI_ERROR_VERSION: return "plugin version mismatch: install skprx from this build";
	default: return "";
	}
}

static void usb_connect(void)
{
	usb_ok = 0;
	if (load_result < 0) return;
	api_version = psvitaUsbAudioMidiGetApiVersion();
	if (api_version != (int)PSVITA_USB_AUDIO_MIDI_API_VERSION) {
		acquire_result = PSVITA_USB_AUDIO_MIDI_ERROR_VERSION;
		return;
	}
	acquire_result = psvitaUsbAudioMidiAcquire(PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP);
	usb_ok = acquire_result >= 0;
}

static void all_notes_off(void)
{
	int n;
	for (n = 0; n < 128; n++) if (own_keys[n]) stop_note(n);
	queue3(0xB0, 64, 0);
	queue3(0xB0, 123, 0);
	flush_out();
}

static void save_report(const PsvitaUsbAudioMidiStatus *st, char *msg, size_t cap)
{
	char b[1024];
	int len, fd, i;
	sceIoMkdir("ux0:data/vitamidi", 0777);
	len = snprintf(b, sizeof(b),
		"VitaMIDI Controller %s\nload 0x%08X api 0x%08X acquire 0x%08X\n"
		"state %u flags 0x%08X last_error 0x%08X rx_dropped %u tx_dropped %u filtered %u\n"
		"sent %u recv %u send_errors %u last_write_error 0x%08X\nlog:\n",
		APP_VERSION, (unsigned)load_result, (unsigned)api_version, (unsigned)acquire_result,
		st->state, st->flags, (unsigned)st->last_error, st->rx_dropped, st->tx_dropped, st->filtered,
		sent_count, recv_count, send_errors, (unsigned)last_write_error);
	for (i = 0; i < log_count && len < (int)sizeof(b) - 64; i++)
		len += snprintf(b + len, sizeof(b) - len, "  %s\n", log_lines[i]);
	fd = sceIoOpen("ux0:data/vitamidi/controller_report.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, b, len);
		sceIoClose(fd);
		snprintf(msg, cap, "saved ux0:data/vitamidi/controller_report.txt");
	} else {
		snprintf(msg, cap, "report save failed 0x%08X", (unsigned)fd);
	}
}

/* ---------- main ---------- */

int main(void)
{
	SceCtrlData pad, old;
	SceTouchData front, back;
	PsvitaUsbAudioMidiStatus st;
	int finger_note[MAX_TOUCH];
	int finger_id[MAX_TOUCH];
	int last_bend = 8192, last_mod = 0, last_cc74 = -1, last_cc71 = -1;
	char line[128], msg[96] = "";
	SceUID th;
	int i;

	for (i = 0; i < VOICES; i++) voices[i].note = -1;
	for (i = 0; i < MAX_TOUCH; i++) { finger_note[i] = -1; finger_id[i] = -1; }
	synth_lock = sceKernelCreateMutex("synth", 0, 0, NULL);

	vita2d_init();
	vita2d_set_clear_color(C_BG);
	font = vita2d_load_default_pgf();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
	memset(&old, 0, sizeof(old));
	memset(&st, 0, sizeof(st));

	load_result = psvitaUsbAudioMidiClientLoad("app0:psvita_usb_audio_midi_client.suprx");
	usb_connect();

	th = sceKernelCreateThread("ctrl_audio", audio_thread, 0x10000100 - 10, 0x10000, 0, 0, NULL);
	if (th >= 0) sceKernelStartThread(th, 0, NULL);

	while (app_running) {
		unsigned pressed, released;
		int connected;

		sceCtrlPeekBufferPositive(0, &pad, 1);
		pressed = pad.buttons & ~old.buttons;
		released = old.buttons & ~pad.buttons;
		old = pad;

		/* --- сенсорные клавиши: каждый палец помнит свою ноту --- */
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &front, 1);
		{
			int seen[MAX_TOUCH] = {0};
			int r;
			for (r = 0; r < (int)front.reportNum && r < MAX_TOUCH; r++) {
				int x = front.report[r].x / 2, y = front.report[r].y / 2, vel = 100;
				int id = front.report[r].id, slot = -1, note;
				for (i = 0; i < MAX_TOUCH; i++) if (finger_id[i] == id) { slot = i; break; }
				if (slot < 0) for (i = 0; i < MAX_TOUCH; i++) if (finger_id[i] < 0) { slot = i; break; }
				if (slot < 0) continue;
				seen[slot] = 1;
				finger_id[slot] = id;
				note = key_at(x, y, &vel);
				if (note > 127) note = -1;
				if (note != finger_note[slot]) {         /* палец съехал на другую клавишу */
					if (finger_note[slot] >= 0) stop_note(finger_note[slot]);
					if (note >= 0) play_note(note, vel);
					finger_note[slot] = note;
				}
			}
			for (i = 0; i < MAX_TOUCH; i++) {
				if (!seen[i] && finger_id[i] >= 0) {
					if (finger_note[i] >= 0) stop_note(finger_note[i]);
					finger_note[i] = -1;
					finger_id[i] = -1;
				}
			}
		}

		/* --- кнопки --- */
		if (pressed & SCE_CTRL_START) app_running = 0;
		if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
			for (i = 0; i < MAX_TOUCH; i++) {     /* при смене октавы гасим зажатые ноты */
				if (finger_note[i] >= 0) stop_note(finger_note[i]);
				finger_note[i] = -1;
			}
			if ((pressed & SCE_CTRL_LTRIGGER) && octave > 0) octave--;
			if ((pressed & SCE_CTRL_RTRIGGER) && octave < 8) octave++;
		}
		if (pressed & SCE_CTRL_TRIANGLE) { queue3(0xB0, 64, 127); log_add("-> SUSTAIN ON"); }
		if (released & SCE_CTRL_TRIANGLE) { queue3(0xB0, 64, 0); log_add("-> SUSTAIN OFF"); }
		if (pressed & SCE_CTRL_SELECT) { all_notes_off(); channel = (channel + 1) & 15; }
		if (pressed & SCE_CTRL_CIRCLE) {
			synth_on = !synth_on;
			if (!synth_on) for (i = 0; i < 128; i++) synth_note_off(i);
		}
		if (pressed & SCE_CTRL_CROSS) {
			if (usb_ok) { all_notes_off(); psvitaUsbAudioMidiRelease(); }
			usb_connect();
			snprintf(msg, sizeof(msg), "reconnect: 0x%08X", (unsigned)acquire_result);
		}

		/* --- стики: pitch bend и модуляция, отправляем только изменения --- */
		{
			int lx = pad.lx - 128, bend, mod;
			if (lx > -12 && lx < 12) lx = 0;           /* мёртвая зона */
			bend = 8192 + lx * 64;
			if (bend < 0) bend = 0;
			if (bend > 16383) bend = 16383;
			if (bend != last_bend) {
				queue3(0xE0, (uint8_t)(bend & 0x7f), (uint8_t)(bend >> 7));
				last_bend = bend;
			}
			mod = 128 - pad.ry;                        /* вверх — больше */
			if (mod < 12) mod = 0;
			mod = mod > 127 ? 127 : mod;
			if (mod != last_mod) { queue3(0xB0, 1, (uint8_t)mod); last_mod = mod; }
		}

		/* --- задняя панель: два контроллера --- */
		sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
		if (back.reportNum > 0) {
			int cx = back.report[0].x * 127 / 1919;
			int cy = 127 - (back.report[0].y - 108) * 127 / 780;
			if (cx < 0) cx = 0;
			if (cx > 127) cx = 127;
			if (cy < 0) cy = 0;
			if (cy > 127) cy = 127;
			if (cx != last_cc74) { queue3(0xB0, 74, (uint8_t)cx); last_cc74 = cx; }
			if (cy != last_cc71) { queue3(0xB0, 71, (uint8_t)cy); last_cc71 = cy; }
		}

		flush_out();

		/* --- входящие события в журнал --- */
		while (evq_tail != evq_head) {
			PsvitaUsbMidiEvent e = evq[evq_tail % EVQ];
			evq_tail++;
			recv_count++;
			if (e.size == 3 && (e.data[0] & 0xf0) == 0x90 && e.data[2]) {
				snprintf(line, sizeof(line), "<- ON  %s%d vel %d", NOTE_NAMES[e.data[1] % 12],
					e.data[1] / 12 - 1, e.data[2]);
				log_add(line);
			} else if (e.size == 1 && e.data[0] != 0xf8) {
				log_add(e.data[0] == 0xfa ? "<- START" : e.data[0] == 0xfc ? "<- STOP" : "<- CONTINUE");
			}
		}

		if (usb_ok) {
			st.size = sizeof(st);
			psvitaUsbAudioMidiGetStatus(&st);
		}
		connected = usb_ok && (st.flags & PSVITA_USB_AUDIO_MIDI_STATUS_CONNECTED);

		/* --- экран --- */
		vita2d_start_drawing();
		vita2d_clear_screen();

		vita2d_pgf_draw_text(font, 16, 30, C_ORANGE, 1.2f, "VITAMIDI CONTROLLER " APP_VERSION);
		snprintf(line, sizeof(line), "CH %d   OCT C%d   SYNTH %s", channel + 1, octave, synth_on ? "ON" : "OFF");
		vita2d_pgf_draw_text(font, 560, 30, C_TEXT, 1.0f, line);

		if (load_result < 0) {
			vita2d_pgf_draw_text(font, 16, 76, C_BAD, 1.0f, "USB MIDI plugin is not loaded.");
			vita2d_pgf_draw_text(font, 16, 102, C_TEXT, 1.0f, "Copy psvita_usb_audio_midi.skprx to ur0:tai/,");
			vita2d_pgf_draw_text(font, 16, 128, C_TEXT, 1.0f, "add it under *KERNEL in ur0:tai/config.txt, reboot.");
			snprintf(line, sizeof(line), "(load 0x%08X)", (unsigned)load_result);
			vita2d_pgf_draw_text(font, 16, 154, C_DIM, 1.0f, line);
		} else if (!usb_ok) {
			snprintf(line, sizeof(line), "USB not acquired: 0x%08X", (unsigned)acquire_result);
			vita2d_pgf_draw_text(font, 16, 76, C_BAD, 1.0f, line);
			vita2d_pgf_draw_text(font, 16, 102, C_TEXT, 1.0f, explain(acquire_result));
			vita2d_pgf_draw_text(font, 16, 128, C_DIM, 1.0f, "Press X to retry");
		} else {
			vita2d_pgf_draw_text(font, 16, 76, connected ? C_OK : C_WARN, 1.0f,
				connected ? "CONNECTED to computer: \"PS Vita USB\"" : "Waiting for computer: plug the USB cable");
			snprintf(line, sizeof(line), "sent %u   received %u   errors %u", sent_count, recv_count, send_errors);
			vita2d_pgf_draw_text(font, 16, 102, C_TEXT, 1.0f, line);
			snprintf(line, sizeof(line), "plugin: filtered %u  tx dropped %u  last 0x%08X",
				st.filtered, st.tx_dropped, (unsigned)st.last_error);
			vita2d_pgf_draw_text(font, 16, 128, C_DIM, 1.0f, line);
			if (send_errors && sent_count == 0)
				vita2d_pgf_draw_text(font, 16, 154, C_BAD, 1.0f,
					"Notes are rejected: install the VitaMIDI build of the plugin");
		}
		vita2d_pgf_draw_text(font, 16, 196, C_DIM, 1.0f, "L/R octave  /\\ sustain  sticks: bend / mod");
		vita2d_pgf_draw_text(font, 16, 220, C_DIM, 1.0f, "rear: CC74 CC71  SELECT ch  O synth  X reconnect");
		vita2d_pgf_draw_text(font, 16, 244, C_DIM, 1.0f, "[] report  START exit");
		if (msg[0]) vita2d_pgf_draw_text(font, 16, 276, C_WARN, 1.0f, msg);

		for (i = 0; i < log_count; i++)
			vita2d_pgf_draw_text(font, 600, 76 + i * 26, i == 0 ? C_TEXT : C_DIM, 1.0f, log_lines[i]);

		draw_keys();

		vita2d_end_drawing();
		vita2d_swap_buffers();

		if (pressed & SCE_CTRL_SQUARE) save_report(&st, msg, sizeof(msg));
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	}

	if (usb_ok) {
		all_notes_off();
		psvitaUsbAudioMidiRelease();
	}
	if (th >= 0) {
		sceKernelWaitThreadEnd(th, NULL, NULL);
		sceKernelDeleteThread(th);
	}
	vita2d_fini();
	vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
