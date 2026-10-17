/*
 * vitamidi.skprx — USB-MIDI хост для PS Vita.
 *
 * Как устроено:
 *  - при запуске регистрирует USB-драйвер класса Audio / MIDIStreaming (SceUsbd);
 *  - приложение вызывает vitamidiStart(): мультиконнектор Vita 1000 переводится
 *    в режим хоста (как в ViXEn и vita-usb-ether), Vita начинает видеть устройства;
 *  - подключённая клавиатура читается bulk-передачами, пакеты USB-MIDI
 *    разбираются в колбэке и складываются в очередь;
 *  - приложение забирает события через vitamidiRead();
 *  - vitamidiStop() или выход/падение приложения возвращают порт в обычный режим.
 *
 * Питание на линию VBUS Vita в режиме хоста не подаёт — нужен Y-кабель или
 * хаб с внешним питанием.
 */
#include <psp2kern/kernel/aimgr.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/usbd.h>
#include <psp2kern/usbserv.h>

#include "vitamidi.h"
#include "umidi.h"

#define MULTICN_PORT 2           /* мультиконнектор Vita 1000 */
#define MAX_CONSECUTIVE_ERRORS 16
#define RX_BIT 1u

int module_start(SceSize argc, const void *args);
int module_stop(SceSize argc, const void *args);
void _start() __attribute__((weak, alias("module_start")));

/* ---------- состояние ---------- */

static SceUID g_lock = -1;        /* старт/стоп/подключение/статус */
static SceUID g_read_lock = -1;   /* один читатель очереди за раз */
static SceUID g_rx_flag = -1;     /* «пришли события» */
static SceUID g_proc_handler = -1;
static SceUID g_sys_handler = -1;
static int g_driver_registered;
static int g_usbcharge_removed;
static int g_is_vita;             /* настоящая портативная Vita */
static int g_is_pstv;

static volatile SceUID g_owner = -1;  /* процесс, включивший хост */
static volatile int g_host_on;

typedef struct MidiDevice {
	volatile int attached;
	volatile uint32_t gen;        /* меняется при каждом подключении/отключении */
	int device_id;
	SceUID pipe_in;
	SceUID pipe_out;
	int in_is_interrupt;
	uint16_t in_size;
	volatile int stalled;
	int consecutive_errors;
} MidiDevice;

static MidiDevice g_dev;
static UmidiRing g_ring;
static UmidiStats g_ustats;
static VitaMidiStatus g_status;
static unsigned char g_in_buf[64] __attribute__((aligned(64)));

static void lock(void) { ksceKernelLockMutex(g_lock, 1, NULL); }
static void unlock(void) { ksceKernelUnlockMutex(g_lock, 1); }

/* ---------- режим хоста ---------- */

static int host_select(int on)
{
	int r;
	if (!g_is_vita)
		return 0; /* PS TV: USB-порты хостовые всегда */
	r = ksceUsbServMacSelect(MULTICN_PORT, on ? 0 : 1);
	ksceDebugPrintf("[vitamidi] MacSelect(%d, %d) = 0x%08X\n", MULTICN_PORT, on ? 0 : 1, r);
	return r;
}

/* ---------- чтение с устройства ---------- */

static void submit_read(uint32_t gen);

static void on_read_done(int32_t result, int32_t count, void *arg)
{
	uint32_t gen = (uint32_t)arg;
	if (!g_dev.attached || gen != g_dev.gen)
		return; /* устройство уже отключено */

	g_status.transfers++;
	if (result == 0) {
		g_dev.consecutive_errors = 0;
		if (count > 0) {
			uint32_t before = g_ustats.events;
			if ((uint32_t)count > sizeof(g_in_buf))
				count = sizeof(g_in_buf);
			umidi_parse(g_in_buf, (uint32_t)count,
				(uint64_t)ksceKernelGetSystemTimeWide(), &g_ring, &g_ustats);
			if (g_ustats.events != before) {
				ksceKernelSetEventFlag(g_rx_flag, RX_BIT);
				ksceKernelPowerTick(0); /* игра на клавишах не даёт Vita уснуть */
			}
		}
	} else {
		g_status.errors++;
		g_status.last_error = result;
		if (++g_dev.consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
			g_dev.stalled = 1;
			ksceDebugPrintf("[vitamidi] too many errors, last 0x%08X\n", result);
			return;
		}
	}
	submit_read(gen);
}

static void submit_read(uint32_t gen)
{
	int r;
	if (!g_dev.attached || gen != g_dev.gen)
		return;
	if (g_dev.in_is_interrupt)
		r = ksceUsbdInterruptTransfer(g_dev.pipe_in, g_in_buf, g_dev.in_size,
			on_read_done, (void *)gen);
	else
		r = ksceUsbdBulkTransfer(g_dev.pipe_in, g_in_buf, g_dev.in_size,
			on_read_done, (void *)gen);
	if (r < 0) {
		g_status.errors++;
		g_status.last_error = r;
		g_dev.stalled = 1;
		ksceDebugPrintf("[vitamidi] transfer submit error 0x%08X\n", r);
	}
}

/* ---------- драйвер USB ---------- */

/* Ищем интерфейс Audio / MIDIStreaming с конечными точками. */
static SceUsbdInterfaceDescriptor *find_midi_interface(int device_id, uint8_t *last_class)
{
	SceUsbdInterfaceDescriptor *i;
	i = (SceUsbdInterfaceDescriptor *)ksceUsbdScanStaticDescriptor(device_id, NULL,
		SCE_USBD_DESCRIPTOR_INTERFACE);
	while (i) {
		if (last_class)
			*last_class = i->bInterfaceClass;
		if (i->bInterfaceClass == SCE_USBD_CLASS_AUDIO && i->bInterfaceSubclass == 0x03 &&
		    i->bNumEndpoints > 0)
			return i;
		i = (SceUsbdInterfaceDescriptor *)ksceUsbdScanStaticDescriptor(device_id, i,
			SCE_USBD_DESCRIPTOR_INTERFACE);
	}
	return NULL;
}

static int vitamidi_probe(int device_id)
{
	SceUsbdDeviceDescriptor *d;
	uint8_t cls = 0xff;
	int is_midi;

	d = (SceUsbdDeviceDescriptor *)ksceUsbdScanStaticDescriptor(device_id, NULL,
		SCE_USBD_DESCRIPTOR_DEVICE);
	is_midi = find_midi_interface(device_id, &cls) != NULL;

	g_status.devices_seen++;
	if (d) {
		g_status.seen_vid = d->idVendor;
		g_status.seen_pid = d->idProduct;
	}
	g_status.seen_class = is_midi ? SCE_USBD_CLASS_AUDIO : cls;
	g_status.seen_is_midi = (uint8_t)is_midi;
	ksceDebugPrintf("[vitamidi] probe %04X:%04X midi=%d\n",
		d ? d->idVendor : 0, d ? d->idProduct : 0, is_midi);

	return is_midi ? SCE_USBD_PROBE_SUCCEEDED : SCE_USBD_PROBE_FAILED;
}

static int attach_locked(int device_id)
{
	SceUsbdDeviceDescriptor *d;
	SceUsbdConfigurationDescriptor *cfg;
	SceUsbdInterfaceDescriptor *iface;
	SceUsbdEndpointDescriptor *ep;
	SceUsbdEndpointDescriptor *ep_in = NULL, *ep_out = NULL;
	SceUID ctrl;
	int n, r;

	if (g_dev.attached)
		return -1; /* пока поддерживаем одно устройство */

	d = (SceUsbdDeviceDescriptor *)ksceUsbdScanStaticDescriptor(device_id, NULL,
		SCE_USBD_DESCRIPTOR_DEVICE);
	cfg = (SceUsbdConfigurationDescriptor *)ksceUsbdScanStaticDescriptor(device_id, NULL,
		SCE_USBD_DESCRIPTOR_CONFIGURATION);
	iface = find_midi_interface(device_id, NULL);
	if (!d || !cfg || !iface)
		return -2;

	/* Конечные точки MIDIStreaming идут сразу за интерфейсом. */
	ep = (SceUsbdEndpointDescriptor *)ksceUsbdScanStaticDescriptor(device_id, iface,
		SCE_USBD_DESCRIPTOR_ENDPOINT);
	for (n = 0; ep && n < iface->bNumEndpoints; n++) {
		int type = ep->bmAttributes & SCE_USBD_ENDPOINT_TRANSFER_TYPE_BITS;
		if (type == SCE_USBD_ENDPOINT_TRANSFER_TYPE_BULK ||
		    type == SCE_USBD_ENDPOINT_TRANSFER_TYPE_INTERRUPT) {
			if ((ep->bEndpointAddress & SCE_USBD_ENDPOINT_DIRECTION_BITS) ==
			    SCE_USBD_ENDPOINT_DIRECTION_IN) {
				if (!ep_in) ep_in = ep;
			} else if (!ep_out) {
				ep_out = ep;
			}
		}
		ep = (SceUsbdEndpointDescriptor *)ksceUsbdScanStaticDescriptor(device_id, ep,
			SCE_USBD_DESCRIPTOR_ENDPOINT);
	}
	if (!ep_in)
		return -3;

	g_dev.pipe_in = ksceUsbdOpenPipe(device_id, ep_in);
	if (g_dev.pipe_in <= 0)
		return g_dev.pipe_in < 0 ? g_dev.pipe_in : -4;
	g_dev.pipe_out = ep_out ? ksceUsbdOpenPipe(device_id, ep_out) : 0;
	if (g_dev.pipe_out < 0)
		g_dev.pipe_out = 0; /* выход пока не нужен */

	ctrl = ksceUsbdOpenPipe(device_id, NULL);
	if (ctrl <= 0) {
		ksceUsbdClosePipe(g_dev.pipe_in);
		g_dev.pipe_in = 0;
		return ctrl < 0 ? ctrl : -5;
	}
	r = ksceUsbdSetConfiguration(ctrl, cfg->bConfigurationValue, NULL, NULL);
	ksceDebugPrintf("[vitamidi] SetConfiguration(%d) = 0x%08X\n", cfg->bConfigurationValue, r);
	if (r < 0) {
		ksceUsbdClosePipe(g_dev.pipe_in);
		g_dev.pipe_in = 0;
		return r;
	}

	g_dev.device_id = device_id;
	g_dev.in_is_interrupt = (ep_in->bmAttributes & SCE_USBD_ENDPOINT_TRANSFER_TYPE_BITS) ==
		SCE_USBD_ENDPOINT_TRANSFER_TYPE_INTERRUPT;
	g_dev.in_size = ep_in->wMaxPacketSize & 0x7ff;
	if (g_dev.in_size == 0 || g_dev.in_size > sizeof(g_in_buf))
		g_dev.in_size = sizeof(g_in_buf);
	g_dev.stalled = 0;
	g_dev.consecutive_errors = 0;

	g_status.vid = d->idVendor;
	g_status.pid = d->idProduct;
	g_status.ep_in = ep_in->bEndpointAddress;
	g_status.ep_out = ep_out ? ep_out->bEndpointAddress : 0;
	g_status.max_packet = g_dev.in_size;

	g_dev.gen++;
	g_dev.attached = 1;
	submit_read(g_dev.gen);
	return 0;
}

static int vitamidi_attach(int device_id)
{
	int r;
	lock();
	r = attach_locked(device_id);
	if (r != 0)
		g_status.attach_result = r;
	unlock();
	ksceDebugPrintf("[vitamidi] attach %d = %d\n", device_id, r);
	return r == 0 ? SCE_USBD_ATTACH_SUCCEEDED : SCE_USBD_ATTACH_FAILED;
}

static int vitamidi_detach(int device_id)
{
	int r = SCE_USBD_DETACH_FAILED;
	lock();
	if (g_dev.attached && g_dev.device_id == device_id) {
		g_dev.attached = 0;
		g_dev.gen++;
		g_dev.pipe_in = 0;
		g_dev.pipe_out = 0;
		r = SCE_USBD_DETACH_SUCCEEDED;
	}
	unlock();
	ksceDebugPrintf("[vitamidi] detach %d\n", device_id);
	return r;
}

static const SceUsbdDriver g_driver = {
	.name = "vitamidi",
	.probe = vitamidi_probe,
	.attach = vitamidi_attach,
	.detach = vitamidi_detach,
};

/* Системный драйвер usb_charge перехватывает часть устройств (см. ViXEn).
 * Подменяем его пустышкой с тем же именем и снимаем с регистрации. */
static int fake_probe(int id) { (void)id; return SCE_USBD_PROBE_FAILED; }
static int fake_attach(int id) { (void)id; return SCE_USBD_ATTACH_FAILED; }
static int fake_detach(int id) { (void)id; return SCE_USBD_DETACH_FAILED; }
static const SceUsbdDriver g_fake_usbcharge = {
	.name = "usb_charge",
	.probe = fake_probe,
	.attach = fake_attach,
	.detach = fake_detach,
};

/* ---------- включение/выключение ---------- */

static void stop_locked(void)
{
	if (g_host_on) {
		g_status.host_result = host_select(0);
		g_host_on = 0;
	}
	if (g_dev.attached) {
		g_dev.attached = 0;
		g_dev.gen++;
	}
	g_owner = -1;
}

static int proc_exit(SceUID pid, SceProcEventInvokeParam1 *param, int arg)
{
	(void)param; (void)arg;
	if (pid == g_owner) {
		lock();
		if (pid == g_owner)
			stop_locked();
		unlock();
	}
	return 0;
}

static const SceProcEventHandler g_proc = {
	.size = sizeof(SceProcEventHandler),
	.create = NULL,
	.exit = proc_exit,
	.kill = proc_exit,
	.stop = NULL,
	.start = NULL,
	.switch_process = NULL,
};

static int sys_event(int resume, int eventid, void *args, void *opt)
{
	(void)eventid; (void)args; (void)opt;
	if (resume && g_host_on)
		host_select(1); /* после сна режим хоста сбрасывается */
	return 0;
}

/* ---------- системные вызовы ---------- */

int vitamidiGetVersion(void)
{
	return VITAMIDI_API_VERSION;
}

int vitamidiStart(void)
{
	unsigned long state;
	SceUID pid;
	int r = 0;

	ENTER_SYSCALL(state);
	pid = ksceKernelGetProcessId();
	lock();
	if (g_owner >= 0 && g_owner != pid) {
		r = VITAMIDI_ERROR_BUSY;
	} else if (!g_host_on) {
		if (g_is_vita && !g_usbcharge_removed) {
			int u = ksceUsbdUnregisterDriver(&g_fake_usbcharge);
			ksceDebugPrintf("[vitamidi] unregister usb_charge = 0x%08X\n", u);
			g_usbcharge_removed = 1;
		}
		umidi_ring_reset(&g_ring);
		g_ustats.packets = g_ustats.events = g_ustats.dropped = 0;
		g_status.host_result = host_select(1);
		if (g_status.host_result < 0) {
			r = VITAMIDI_ERROR_HOST;
		} else {
			g_host_on = 1;
			g_owner = pid;
		}
	}
	unlock();
	EXIT_SYSCALL(state);
	return r;
}

int vitamidiStop(void)
{
	unsigned long state;
	int r = 0;
	ENTER_SYSCALL(state);
	lock();
	if (g_owner >= 0 && g_owner != ksceKernelGetProcessId())
		r = VITAMIDI_ERROR_NOT_OWNER;
	else
		stop_locked();
	unlock();
	ksceKernelSetEventFlag(g_rx_flag, RX_BIT); /* разбудить ждущего читателя */
	EXIT_SYSCALL(state);
	return r;
}

int vitamidiGetStatus(VitaMidiStatus *user_status)
{
	unsigned long state;
	uint32_t size = 0;
	VitaMidiStatus st;
	int r = 0;

	ENTER_SYSCALL(state);
	if (!user_status ||
	    ksceKernelMemcpyUserToKernel(&size, user_status, sizeof(size)) < 0 ||
	    size < 8) {
		r = VITAMIDI_ERROR_PARAM;
		goto out;
	}
	if (size > sizeof(st))
		size = sizeof(st);

	lock();
	st = g_status;
	unlock();
	st.size = sizeof(st);
	st.api_version = VITAMIDI_API_VERSION;
	st.state = 0;
	if (g_host_on || g_is_pstv) st.state |= VITAMIDI_STATE_HOST_ON;
	if (g_dev.attached) st.state |= VITAMIDI_STATE_ATTACHED;
	if (g_dev.attached && g_dev.stalled) st.state |= VITAMIDI_STATE_STALLED;
	if (g_is_pstv) st.state |= VITAMIDI_STATE_PSTV;
	st.packets = g_ustats.packets;
	st.events = g_ustats.events;
	st.dropped = g_ustats.dropped;

	if (ksceKernelMemcpyKernelToUser(user_status, &st, size) < 0)
		r = VITAMIDI_ERROR_PARAM;
out:
	EXIT_SYSCALL(state);
	return r;
}

int vitamidiRead(VitaMidiEvent *user_events, uint32_t max, uint32_t timeout_us)
{
	unsigned long state;
	VitaMidiEvent ev[VITAMIDI_MAX_READ];
	uint32_t n = 0;
	int r;

	ENTER_SYSCALL(state);
	if (!user_events || max == 0 || timeout_us > 1000000u) {
		r = VITAMIDI_ERROR_PARAM;
		goto out;
	}
	if (max > VITAMIDI_MAX_READ)
		max = VITAMIDI_MAX_READ;
	if (g_owner >= 0 && g_owner != ksceKernelGetProcessId()) {
		r = VITAMIDI_ERROR_NOT_OWNER;
		goto out;
	}

	ksceKernelLockMutex(g_read_lock, 1, NULL);
	n = umidi_ring_pop(&g_ring, ev, max);
	ksceKernelUnlockMutex(g_read_lock, 1);

	if (n == 0 && timeout_us > 0) {
		unsigned int bits = 0;
		SceUInt t = timeout_us;
		int w = ksceKernelWaitEventFlag(g_rx_flag, RX_BIT,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &t);
		if (w >= 0) {
			ksceKernelLockMutex(g_read_lock, 1, NULL);
			n = umidi_ring_pop(&g_ring, ev, max);
			ksceKernelUnlockMutex(g_read_lock, 1);
		}
	} else if (n > 0) {
		ksceKernelClearEventFlag(g_rx_flag, ~RX_BIT);
	}

	r = (int)n;
	if (n && ksceKernelMemcpyKernelToUser(user_events, ev,
			n * sizeof(VitaMidiEvent)) < 0)
		r = VITAMIDI_ERROR_PARAM;
out:
	EXIT_SYSCALL(state);
	return r;
}

/* ---------- загрузка модуля ---------- */

int module_start(SceSize argc, const void *args)
{
	int r;
	(void)argc; (void)args;

	memset(&g_dev, 0, sizeof(g_dev));
	memset(&g_status, 0, sizeof(g_status));
	umidi_ring_reset(&g_ring);

	g_is_vita = ksceSblAimgrIsGenuineVITA();
	g_is_pstv = ksceSblAimgrIsGenuineDolce();

	g_lock = ksceKernelCreateMutex("vitamidi_lock", 0, 0, NULL);
	g_read_lock = ksceKernelCreateMutex("vitamidi_read", 0, 0, NULL);
	g_rx_flag = ksceKernelCreateEventFlag("vitamidi_rx", SCE_EVENT_WAITMULTIPLE, 0, NULL);
	if (g_lock < 0 || g_read_lock < 0 || g_rx_flag < 0)
		goto fail;

	r = ksceUsbdRegisterDriver(&g_driver);
	ksceDebugPrintf("[vitamidi] RegisterDriver = 0x%08X (vita=%d pstv=%d)\n", r, g_is_vita, g_is_pstv);
	if (r < 0)
		goto fail;
	g_driver_registered = 1;

	g_proc_handler = ksceKernelRegisterProcEventHandler("vitamidi_proc", &g_proc, 0);
	g_sys_handler = ksceKernelRegisterSysEventHandler("zvitamidi_sysevent", sys_event, NULL);
	return SCE_KERNEL_START_SUCCESS;

fail:
	if (g_rx_flag >= 0) ksceKernelDeleteEventFlag(g_rx_flag);
	if (g_read_lock >= 0) ksceKernelDeleteMutex(g_read_lock);
	if (g_lock >= 0) ksceKernelDeleteMutex(g_lock);
	g_lock = g_read_lock = g_rx_flag = -1;
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	lock();
	stop_locked();
	unlock();
	/* обработчик сна не снимаем: в vitasdk нет ksceKernelUnregisterSysEventHandler;
	 * плагины из config.txt не выгружаются до перезагрузки */
	if (g_proc_handler >= 0) ksceKernelUnregisterProcEventHandler(g_proc_handler);
	if (g_driver_registered) ksceUsbdUnregisterDriver(&g_driver);
	ksceKernelDeleteEventFlag(g_rx_flag);
	ksceKernelDeleteMutex(g_read_lock);
	ksceKernelDeleteMutex(g_lock);
	return SCE_KERNEL_STOP_SUCCESS;
}
