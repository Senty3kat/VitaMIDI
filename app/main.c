/*
 * VitaMIDI Test — проверка плагина vitamidi.skprx.
 *
 * Показывает по шагам: установлен ли плагин, включён ли хост, видит ли Vita
 * устройство на шине, подключилась ли MIDI-клавиатура, приходят ли ноты.
 * Ноты звучат треугольной волной (8 голосов), чтобы оценить задержку на слух.
 *
 * Кнопки: X — хост вкл/выкл, КВАДРАТ — сохранить отчёт, ТРЕУГОЛЬНИК — очистить
 * журнал, START — выход.
 */
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "vitamidi.h"

#define APP_VERSION "0.1"
#define SR 48000
#define BLOCK 256
#define VOICES 8
#define LOG_LINES 13

/* ---------- синт ---------- */

typedef struct Voice {
	int note;       /* -1 — свободен */
	float phase, inc;
	float amp, target;
	unsigned age;
} Voice;

static Voice voices[VOICES];
static unsigned voice_clock;
static volatile int keys[128];       /* сила нажатия для экрана (0 — отпущена) */
static volatile int app_running = 1;
static volatile int midi_ok;          /* плагин найден */

/* Журнал событий: аудиопоток пишет, экран читает. */
#define EVQ 256
static VitaMidiEvent evq[EVQ];
static volatile unsigned evq_head;
static unsigned evq_tail;

static volatile unsigned latency_us;  /* от прихода пакета до обработки в аудиопотоке */

static void note_on(int note, int vel)
{
	int i, best = 0;
	for (i = 0; i < VOICES; i++) {
		if (voices[i].note == note) { best = i; goto use; }
	}
	for (i = 0; i < VOICES; i++) {
		if (voices[i].note < 0 && voices[i].amp < 0.0001f) { best = i; goto use; }
	}
	for (i = 1; i < VOICES; i++)          /* все заняты — берём самый старый */
		if (voices[i].age < voices[best].age) best = i;
use:
	voices[best].note = note;
	voices[best].inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / SR;
	voices[best].target = 0.04f + 0.16f * vel / 127.0f;
	voices[best].age = ++voice_clock;
}

static void note_off(int note)
{
	int i;
	for (i = 0; i < VOICES; i++)
		if (voices[i].note == note) { voices[i].note = -1; voices[i].target = 0; }
}

static void handle_event(const VitaMidiEvent *e)
{
	uint8_t st = e->data[0] & 0xf0;
	if (e->size == 3 && st == 0x90 && e->data[2] > 0) {
		note_on(e->data[1], e->data[2]);
		keys[e->data[1]] = e->data[2];
	} else if (e->size == 3 && (st == 0x80 || st == 0x90)) {
		note_off(e->data[1]);
		keys[e->data[1]] = 0;
	} else if (e->size == 3 && st == 0xB0 && (e->data[1] == 120 || e->data[1] == 123)) {
		int i;
		for (i = 0; i < 128; i++) { note_off(i); keys[i] = 0; }
	}
}

static int audio_thread(SceSize args, void *argp)
{
	static int16_t buf[2][BLOCK * 2];
	int which = 0;
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, BLOCK, SR,
		SCE_AUDIO_OUT_MODE_STEREO);
	(void)args; (void)argp;
	if (port < 0)
		return 0;

	while (app_running) {
		int i, v;
		if (midi_ok) {
			VitaMidiEvent ev[VITAMIDI_MAX_READ];
			int n = vitamidiRead(ev, VITAMIDI_MAX_READ, 0);
			if (n > 0) {
				SceInt64 now = sceKernelGetSystemTimeWide();
				latency_us = (unsigned)(now - (SceInt64)ev[n - 1].timestamp_us);
			}
			for (i = 0; i < n; i++) {
				handle_event(&ev[i]);
				evq[evq_head % EVQ] = ev[i];
				evq_head++;
			}
		}
		for (i = 0; i < BLOCK; i++) {
			float s = 0;
			for (v = 0; v < VOICES; v++) {
				Voice *o = &voices[v];
				float tri;
				/* плавная атака и затухание без щелчков */
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
		sceAudioOutOutput(port, buf[which]);
		which ^= 1;
	}
	sceAudioOutReleasePort(port);
	return 0;
}

/* ---------- текст событий ---------- */

static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void describe(const VitaMidiEvent *e, char *out, size_t cap)
{
	uint8_t st = e->data[0] & 0xf0, ch = (e->data[0] & 0x0f) + 1;
	int n = e->data[1];
	switch (st) {
	case 0x90:
		if (e->data[2]) {
			snprintf(out, cap, "NOTE ON   %s%d  vel %3d  ch %d", NOTE_NAMES[n % 12], n / 12 - 1, e->data[2], ch);
			return;
		}
		/* fallthrough: Note On с нулевой силой = Note Off */
	case 0x80:
		snprintf(out, cap, "NOTE OFF  %s%d  ch %d", NOTE_NAMES[n % 12], n / 12 - 1, ch);
		return;
	case 0xB0: snprintf(out, cap, "CC %3d = %3d  ch %d", e->data[1], e->data[2], ch); return;
	case 0xE0: snprintf(out, cap, "PITCH %+5d  ch %d", ((e->data[2] << 7) | e->data[1]) - 8192, ch); return;
	case 0xC0: snprintf(out, cap, "PROGRAM %d  ch %d", e->data[1], ch); return;
	case 0xD0: snprintf(out, cap, "PRESSURE %d  ch %d", e->data[1], ch); return;
	case 0xA0: snprintf(out, cap, "POLY AT %d = %d  ch %d", e->data[1], e->data[2], ch); return;
	default:
		snprintf(out, cap, "RAW %d: %02X %02X %02X  cable %d", e->size, e->data[0], e->data[1], e->data[2], e->cable);
	}
}

/* ---------- экран ---------- */

#define C_BG     RGBA8(0x1e, 0x1b, 0x18, 0xff)
#define C_TEXT   RGBA8(0xf2, 0xe8, 0xd5, 0xff)
#define C_DIM    RGBA8(0x8c, 0x82, 0x72, 0xff)
#define C_OK     RGBA8(0x8f, 0xd6, 0x7a, 0xff)
#define C_WARN   RGBA8(0xf2, 0xb2, 0x4b, 0xff)
#define C_BAD    RGBA8(0xf0, 0x6a, 0x50, 0xff)
#define C_ORANGE RGBA8(0xf2, 0x8c, 0x28, 0xff)

static vita2d_pgf *font;

static void text(int x, int y, unsigned color, const char *s)
{
	vita2d_pgf_draw_text(font, x, y, color, 1.0f, s);
}

static void step(int y, int state, const char *label, const char *detail)
{
	unsigned c = state > 0 ? C_OK : (state == 0 ? C_WARN : C_DIM);
	vita2d_draw_rectangle(24, y - 14, 14, 14, c);
	text(48, y, C_TEXT, label);
	text(330, y, state > 0 ? C_TEXT : C_DIM, detail);
}

static void draw_keyboard(int x0, int y0, int w, int h)
{
	/* 5 октав: C2 (36) .. C7 (96) */
	const int lo = 36, hi = 96;
	int whites = 0, n, wx = 0;
	float kw;
	for (n = lo; n <= hi; n++) {
		int pc = n % 12;
		if (!(pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)) whites++;
	}
	kw = (float)w / whites;
	for (n = lo; n <= hi; n++) {
		int pc = n % 12;
		if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) continue;
		vita2d_draw_rectangle(x0 + wx * kw, y0, kw - 2, h,
			keys[n] ? C_ORANGE : (n == 60 ? RGBA8(0xd8, 0xcd, 0xb8, 0xff) : C_TEXT));
		wx++;
	}
	wx = 0;
	for (n = lo; n <= hi; n++) {
		int pc = n % 12;
		if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) {
			vita2d_draw_rectangle(x0 + wx * kw - kw * 0.3f, y0, kw * 0.6f, h * 0.6f,
				keys[n] ? C_ORANGE : RGBA8(0x26, 0x22, 0x1e, 0xff));
		} else {
			wx++;
		}
	}
}

/* ---------- отчёт ---------- */

static char log_lines[LOG_LINES][64];
static int log_count;

static void save_report(const VitaMidiStatus *st, int version, int start_result, char *msg, size_t cap)
{
	char b[1024];
	int len, fd, i;
	sceIoMkdir("ux0:data/vitamidi", 0777);
	len = snprintf(b, sizeof(b),
		"VitaMIDI Test %s\nplugin version: %d (0x%08X)\nstart result: 0x%08X\n"
		"state: 0x%X host_result: 0x%08X\n"
		"devices_seen: %u last seen %04X:%04X class %u midi %u\n"
		"attached: %04X:%04X ep_in %02X ep_out %02X max_packet %u\n"
		"transfers %u packets %u events %u dropped %u errors %u last_error 0x%08X attach_result %d\n"
		"last latency us: %u\nlast events:\n",
		APP_VERSION, version, (unsigned)version, (unsigned)start_result,
		st->state, (unsigned)st->host_result,
		st->devices_seen, st->seen_vid, st->seen_pid, st->seen_class, st->seen_is_midi,
		st->vid, st->pid, st->ep_in, st->ep_out, st->max_packet,
		st->transfers, st->packets, st->events, st->dropped, st->errors,
		(unsigned)st->last_error, st->attach_result, latency_us);
	for (i = 0; i < log_count && len < (int)sizeof(b) - 70; i++)
		len += snprintf(b + len, sizeof(b) - len, "  %s\n", log_lines[i]);
	fd = sceIoOpen("ux0:data/vitamidi/report.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, b, len);
		sceIoClose(fd);
		snprintf(msg, cap, "report saved: ux0:data/vitamidi/report.txt");
	} else {
		snprintf(msg, cap, "report save failed 0x%08X", (unsigned)fd);
	}
}

/* ---------- main ---------- */

int main(void)
{
	SceCtrlData pad, old;
	VitaMidiStatus st;
	int version, start_result = 0, i;
	char line[128], msg[96] = "";
	SceUID th;

	for (i = 0; i < VOICES; i++) voices[i].note = -1;

	vita2d_init();
	vita2d_set_clear_color(C_BG);
	font = vita2d_load_default_pgf();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	memset(&old, 0, sizeof(old));
	memset(&st, 0, sizeof(st));

	version = vitamidiGetVersion();
	midi_ok = version == VITAMIDI_API_VERSION;
	if (midi_ok)
		start_result = vitamidiStart();

	th = sceKernelCreateThread("vitamidi_audio", audio_thread, 0x10000100 - 10, 0x10000, 0, 0, NULL);
	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);

	while (app_running) {
		unsigned pressed;
		int host_on, attached;

		sceCtrlPeekBufferPositive(0, &pad, 1);
		pressed = pad.buttons & ~old.buttons;
		old = pad;

		if (midi_ok) {
			st.size = sizeof(st);
			vitamidiGetStatus(&st);
		}
		host_on = (st.state & VITAMIDI_STATE_HOST_ON) != 0;
		attached = (st.state & VITAMIDI_STATE_ATTACHED) != 0;

		if (pressed & SCE_CTRL_START) app_running = 0;
		if (midi_ok && (pressed & SCE_CTRL_CROSS)) {
			if (host_on) { vitamidiStop(); snprintf(msg, sizeof(msg), "host OFF: USB to PC works again"); }
			else { start_result = vitamidiStart(); snprintf(msg, sizeof(msg), "host ON: 0x%08X", (unsigned)start_result); }
		}
		if (pressed & SCE_CTRL_TRIANGLE) log_count = 0;
		if (pressed & SCE_CTRL_SQUARE) save_report(&st, version, start_result, msg, sizeof(msg));

		/* новые события — в журнал (сверху самые свежие) */
		while (evq_tail != evq_head) {
			VitaMidiEvent e = evq[evq_tail % EVQ];
			evq_tail++;
			if ((e.data[0] & 0xf0) == 0xf0 && e.size == 1) continue; /* клок не засоряет журнал */
			memmove(log_lines[1], log_lines[0], sizeof(log_lines[0]) * (LOG_LINES - 1));
			describe(&e, log_lines[0], sizeof(log_lines[0]));
			if (log_count < LOG_LINES) log_count++;
		}

		vita2d_start_drawing();
		vita2d_clear_screen();

		text(24, 36, C_ORANGE, "VITAMIDI TEST " APP_VERSION);
		text(260, 36, C_DIM, "X host on/off   [] save report   /\\ clear   START exit");

		if (!midi_ok) {
			step(80, -1, "1. Plugin vitamidi.skprx", "NOT LOADED");
			text(48, 120, C_BAD, "Copy vitamidi.skprx to ur0:tai/, add the line");
			text(48, 146, C_TEXT, "ur0:tai/vitamidi.skprx");
			text(48, 172, C_BAD, "under *KERNEL in ur0:tai/config.txt and reboot.");
			snprintf(line, sizeof(line), "(vitamidiGetVersion = 0x%08X)", (unsigned)version);
			text(48, 210, C_DIM, line);
		} else {
			snprintf(line, sizeof(line), "API v%d", version);
			step(80, 1, "1. Plugin loaded", line);

			if (start_result < 0)
				snprintf(line, sizeof(line), "start failed 0x%08X", (unsigned)start_result);
			else if (st.state & VITAMIDI_STATE_PSTV)
				snprintf(line, sizeof(line), "PS TV (ports are host)");
			else
				snprintf(line, sizeof(line), host_on ? "ON (MacSelect 0x%08X)" : "OFF  press X", (unsigned)st.host_result);
			step(108, host_on ? 1 : 0, "2. USB host mode", line);

			if (st.devices_seen)
				snprintf(line, sizeof(line), "%u seen, last %04X:%04X class %u%s", st.devices_seen,
					st.seen_vid, st.seen_pid, st.seen_class, st.seen_is_midi ? " MIDI" : "");
			else
				snprintf(line, sizeof(line), host_on ? "nothing yet: check 5V on VBUS" : "-");
			step(136, st.devices_seen ? 1 : (host_on ? 0 : -1), "3. Device on the bus", line);

			if (attached)
				snprintf(line, sizeof(line), "%04X:%04X  ep %02X  %u bytes%s", st.vid, st.pid,
					st.ep_in, st.max_packet, (st.state & VITAMIDI_STATE_STALLED) ? "  STALLED" : "");
			else if (st.attach_result)
				snprintf(line, sizeof(line), "attach failed %d", st.attach_result);
			else
				snprintf(line, sizeof(line), "-");
			step(164, attached ? 1 : (st.devices_seen ? 0 : -1), "4. MIDI keyboard attached", line);

			snprintf(line, sizeof(line), "%u events  %u dropped  %u err (last 0x%08X)  lat %u us",
				st.events, st.dropped, st.errors, (unsigned)st.last_error, latency_us);
			step(192, st.events ? 1 : (attached ? 0 : -1), "5. Notes arriving", line);
		}

		for (i = 0; i < log_count; i++)
			text(48, 236 + i * 20, i == 0 ? C_TEXT : C_DIM, log_lines[i]);

		draw_keyboard(24, 440, 912, 80);
		if (msg[0]) text(560, 236, C_WARN, msg);

		vita2d_end_drawing();
		vita2d_swap_buffers();
		vita2d_wait_rendering_done();
		if (attached) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	}

	if (midi_ok)
		vitamidiStop();
	if (th >= 0) {
		sceKernelWaitThreadEnd(th, NULL, NULL);
		sceKernelDeleteThread(th);
	}
	vita2d_fini();
	vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
