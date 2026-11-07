
/*
 * CUBS touch (boot-arg cubs_touch=1): the Pixel 11's Synaptics TCM touch controller (S3908) on the always-on DesignWare
 * SSI at 0xda2c000, polled. Chip select is the GPIO pin 23 and the interrupt line pin 54 of the always-on pinctrl
 * (one 4 KiB register page per pin at 0xe500000: +0x08 bit 2 is the output level, +0x1c bit 0 the input level). Linux
 * has left the chip in application mode with touch reports on and the controller powered (the hand-over script pins
 * the SPI and pinctrl devices' runtime PM "on"). A frame is: header read {0xa5, code, len lo, len hi}, then a
 * continued read of round16(len + 3) bytes {0xa5, 0x03, payload..., 0x5a}. Code 0xc6 is the touch frame: contact
 * count at byte 32 and 12-byte contacts from byte 34 {status, x, y, z, sizes[4], pad} (x 0..10799, y 0..24239).
 */
#include <kern/sched_prim.h>
extern void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
extern uint32_t redwood_touch_backlog(void);
extern uint32_t redwood_touch_consumed(void);

#define CT_MAXC 10
#define CT_BUF  2112

static volatile uint32_t *ct_spi, *ct_cs, *ct_irq;
static uint8_t ct_tx[CT_BUF], ct_rx[CT_BUF];
static uint32_t ct_div = 32, ct_mode, ct_nframes, ct_ntouch;
static struct { uint32_t down, x, y; } ct_f[CT_MAXC];

#define CT_SSIENR 0x08
#define CT_SER 0x10
#define CT_BAUDR 0x14
#define CT_TXFTLR 0x18
#define CT_RXFTLR 0x1c
#define CT_SR 0x28
#define CT_DR 0x60

static inline uint32_t ct_r(uint32_t off) { return ct_spi[off / 4]; }
static inline void ct_w(uint32_t off, uint32_t v) { ct_spi[off / 4] = v; }

static void
ct_cs_set(int asserted)
{
	uint32_t v = ct_cs[0x08 / 4];

	v = asserted ? (v & ~4u) : (v | 4u);
	ct_cs[0x08 / 4] = v;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (asserted) {
		delay_for_interval(10, 1000);
	}
}

/* one framed transfer: n bytes out (ct_tx) and in (ct_rx), 8-bit mode 0, polled */
static int
ct_xfer(uint32_t n)
{
	uint32_t itx = 0, irx = 0, spin = 0;

	ct_w(CT_SSIENR, 0);
	ct_w(0x00, 0x01070000);
	ct_w(CT_BAUDR, ct_div);
	ct_w(CT_TXFTLR, 0);
	ct_w(CT_RXFTLR, 0);
	ct_w(CT_SER, 1);
	ct_w(CT_SSIENR, 1);
	ct_cs_set(1);
	while (irx < n) {
		while (itx < n && (ct_r(CT_SR) & 2) && (itx - irx) < 8) {
			ct_w(CT_DR, ct_tx[itx++]);
		}
		while (ct_r(CT_SR) & 8) {
			ct_rx[irx++] = (uint8_t)ct_r(CT_DR);
			spin = 0;
		}
		if (++spin > 4000000) {
			ct_cs_set(0);
			ct_w(CT_SSIENR, 0);
			return -1;
		}
	}
	while (ct_r(CT_SR) & 1) {
		if (++spin > 4000000) {
			break;
		}
	}
	ct_cs_set(0);
	ct_w(CT_SSIENR, 0);
	return 0;
}

static void
ct_sleep_ms(uint32_t ms)
{
	assert_wait_timeout((event_t)&ct_sleep_ms, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
ct_post(uint32_t type, uint32_t i, uint32_t x, uint32_t y)
{
	redwood_touch_post(type, i, (x * 65535u) / 10799u, (y * 65535u) / 24239u);
}

static void
ct_frame(uint32_t n)
{
	uint32_t i, cnt = ct_rx[32];

	if (ct_rx[0] != 0xa5 || ct_rx[1] != 0x03 || n < 34 || cnt > CT_MAXC) {
		return;
	}
	if (34 + 12 * cnt > n) {
		return;
	}
	for (i = 0; i < CT_MAXC; i++) {
		if (i < cnt) {
			const uint8_t *r = &ct_rx[34 + 12 * i];
			uint32_t x = r[1] | (r[2] << 8), y = r[3] | (r[4] << 8);
			if (x > 10799) { x = 10799; }
			if (y > 24239) { y = 24239; }
			if (!ct_f[i].down) {
				ct_f[i].down = 1; ct_f[i].x = x; ct_f[i].y = y;
				ct_post(1, i, x, y);
				ct_ntouch++;
			} else if (ct_f[i].x != x || ct_f[i].y != y) {
				ct_f[i].x = x; ct_f[i].y = y;
				ct_post(2, i, x, y);
			}
		} else if (ct_f[i].down) {
			ct_f[i].down = 0;
			ct_post(3, i, ct_f[i].x, ct_f[i].y);
		}
	}
}

static void
ct_poll_once(void)
{
	uint32_t len, rl, code;

	memset(ct_tx, 0, 4);
	if (ct_xfer(4) != 0) {
		return;
	}
	if (ct_mode >= 2 && ct_nframes < 6) {
		printf("CUBS touch: hdr 0x%08x\n", ct_rx[0] | (ct_rx[1] << 8) | (ct_rx[2] << 16) | ((uint32_t)ct_rx[3] << 24));
	}
	if (ct_rx[0] != 0xa5) {
		return;
	}
	code = ct_rx[1];
	len = ct_rx[2] | (ct_rx[3] << 8);
	if (len == 0) {
		return;
	}
	rl = ((len + 3) + 15) & ~15u;
	if (rl > CT_BUF) {
		return;
	}
	memset(ct_tx, 0, rl);
	if (ct_xfer(rl) != 0) {
		return;
	}
	ct_nframes++;
	if (code == 0xc6) {
		ct_frame(rl);
	}
}

/*
 * Buttons: the power key and the two volume keys are inputs of the same always-on pinctrl as the touch controller's chip select and interrupt (Linux's
 * gpio_keys: pins 114 power, 117 volume down, 118 volume up, active low, pulled up by Linux's pin state, which the hand-over leaves in place). Their input level
 * is bit 0 of the pin's register at +0x1c, one 4 KiB page per pin at 0xe500000. A key counts as changed when two reads 2 ms apart agree; one that reads as held
 * before it has been seen released is not believed. The side button is REDWOOD_BUTTON_LOCK, then volume up and down (sys/redwood_input.h, types 4 and 5).
 * Holding power and volume down for cb_hold_ms (boot-arg cubs_btn_hold, default 3000, 0 = off) asks for a return to Android: the kernel resets the system.
 */
/*
 * Return to Android: Linux keeps running beside XNU, so XNU asks it to reboot cleanly. It writes "BYE1" into word 0 of the page at ring + 0xFF0000 (the same
 * reserved RAM the GPU servers use, PA 0xab2000000 + 0xFF0000, which no server touches); a watcher started by the hand-over script on the Android side polls that
 * word and runs sync; reboot. After the reboot the phone comes up in Android as usual (iOS is started again on request).
 */
static void
cubs_return_to_android(void)
{
	volatile uint32_t *w = (volatile uint32_t *)io_map(0xab2000000ULL + 0xFF0000ULL, 0x1000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);

	if (w == NULL) {
		printf("CUBS buttons: cannot map the return page\n");
		return;
	}
	w[0] = 0x31455942;
	__asm__ volatile ("dsb sy" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)w, 64);
	printf("CUBS buttons: return to Android requested\n");
}

#define CB_N 3
static const struct { uint32_t pin, id; } cb_keys[CB_N] = { { 114, 1 }, { 118, 2 }, { 117, 3 } };
static volatile uint32_t *cb_reg[CB_N];
static uint32_t cb_state[CB_N], cb_seen_up[CB_N], cb_pending[CB_N], cb_ready, cb_hold_ms = 3000, cb_ticks;
static uint64_t cb_hold_start;
static uint64_t cb_poll_start;
static uint32_t cb_bye_sent;
static uint32_t cb_nevents, cb_selfpress, cb_power_post = 1, cb_bye_after;   /* boot-arg cubs_bye_after=S: ask for the return to Android S seconds after the buttons start (test aid) */   /* boot-arg cubs_power_post=0: the power key does not reach iOS (default 1: it does; with the wall clock set the wake works) (it sleeps the display, and the wake does not light the panel again) */   /* boot-arg cubs_selfpress=S: a power press every S seconds (test aid) */

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
static int
cb_uread(task_t t, uint64_t va, uint64_t *out)
{
	ppnum_t pn = pmap_find_phys(t->map->pmap, va);

	if (pn == 0) {
		return 0;
	}
	*out = *(volatile uint64_t *)phystokv(((uint64_t)pn << PAGE_SHIFT) | (va & PAGE_MASK));
	return 1;
}

static void
cb_ustack(task_t t, thread_t th, const char *nm, int pid)
{
	arm_saved_state_t *ss = th->machine.upcb;
	uint64_t pc, lr, fp, nfp, nlr;
	const uint64_t mask = 0x0000007fffffffffULL;
	char line[400];
	int n = 0, i;

	if (ss == NULL || !is_saved_state64(ss)) {
		return;
	}
	pc = get_saved_state_pc(ss);
	lr = get_saved_state_lr(ss);
	fp = get_saved_state_fp(ss);
	n = snprintf(line, sizeof line, "pc %#llx lr %#llx", pc & mask, lr & mask);
	for (i = 0; i < 12 && fp != 0 && (fp & 7) == 0 && n < (int)sizeof line - 24; i++) {
		if (!cb_uread(t, fp, &nfp) || !cb_uread(t, fp + 8, &nlr)) {
			break;
		}
		n += snprintf(line + n, sizeof line - (size_t)n, " %#llx", nlr & mask);
		if (nfp <= fp) {
			break;
		}
		fp = nfp;
	}
	printf("CUBS stack: %s[%d] th %p %s\n", nm, pid, th, line);
}

/* boot-arg cubs_threads_after=S: at S seconds (and 60 s later) say what the threads of mediaserverd, SpringBoard and backboardd wait for (debug aid for the audio-server freeze) */
extern queue_head_t tasks;
extern void proc_name(int pid, char *buf, int size);
extern int task_pid(task_t task);
extern vm_offset_t vm_kernel_slide;
static uint32_t cb_threads_after;

static void
cubs_threads_dump(void)
{
	task_t t;
	thread_t th;

	printf("CUBS threads: kernel slide %#lx\n", (unsigned long)vm_kernel_slide);
	queue_iterate(&tasks, t, task_t, tasks) {
		int pid = task_pid(t);
		char nm[40] = "?";

		proc_name(pid, nm, sizeof nm);
		if (strcmp(nm, "mediaserverd") && strcmp(nm, "SpringBoard") && strcmp(nm, "backboardd") && strncmp(nm, "launchd", 7) && strcmp(nm, "runningboardd") && strcmp(nm, "lsd")) {
			continue;
		}
		queue_iterate(&t->threads, th, thread_t, task_threads) {
			printf("CUBS threads: %s[%d] th %p state %#x ev %p cont %p hint %d\n", nm, pid, th, (unsigned)th->state, (void *)th->wait_event,
			    (void *)th->continuation, (int)th->block_hint);
			cb_ustack(t, th, nm, pid);
		}
	}
}

static void
cb_setup(void)
{
	uint32_t i;

	for (i = 0; i < CB_N; i++) {
		cb_reg[i] = (volatile uint32_t *)ml_io_map(0xe500000ULL + (uint64_t)cb_keys[i].pin * 0x1000ULL, 0x1000);
		if (cb_reg[i] == NULL) {
			printf("CUBS buttons: map of pin %u failed\n", cb_keys[i].pin);
			return;
		}
	}
	printf("CUBS buttons: levels power %u vol-up %u vol-down %u (1 = released)\n", cb_reg[0][0x1c / 4] & 1, cb_reg[1][0x1c / 4] & 1, cb_reg[2][0x1c / 4] & 1);
	cb_ready = 1;
}

static void
cb_poll(void)
{
	uint32_t i;
	uint64_t now, elapsed_ns = 0;

	if (!cb_ready) {
		return;
	}
	now = mach_absolute_time();
	if (!cb_poll_start) cb_poll_start = now;
	else absolutetime_to_nanoseconds(now - cb_poll_start, &elapsed_ns);
	cb_ticks++;
	if (cb_threads_after && (cb_ticks == cb_threads_after * 500 || cb_ticks == (cb_threads_after + 60) * 500)) {
		cubs_threads_dump();
	}
	if (cb_bye_after && !cb_bye_sent && elapsed_ns / NSEC_PER_SEC >= cb_bye_after) {
		cb_bye_sent = 1;
		printf("CUBS buttons: test return to Android\n");
		cubs_return_to_android();
	}
	if (cb_selfpress) {
		uint32_t ph = cb_ticks % (cb_selfpress * 500);

		if (cb_ticks >= cb_selfpress * 500 && ph == 0) {
			redwood_touch_post(4, 1, 0, 0);
			printf("CUBS buttons: selfpress down\n");
		} else if (cb_ticks >= cb_selfpress * 500 && ph == 75) {
			redwood_touch_post(5, 1, 0, 0);
			printf("CUBS buttons: selfpress up\n");
		}
	}
	for (i = 0; i < CB_N; i++) {
		uint32_t pressed = (cb_reg[i][0x1c / 4] & 1) == 0;

		if (pressed != cb_pending[i]) {
			printf("CUBS buttons: raw key %u level changed, now %s (reg 0x%x)\n", cb_keys[i].id, pressed ? "low" : "high", cb_reg[i][0x1c / 4]);
		}
		if (pressed == cb_pending[i]) {
			if (!pressed) {
				cb_seen_up[i] = 1;
			}
			if (pressed != cb_state[i] && (cb_seen_up[i] || !pressed)) {
				cb_state[i] = pressed;
				cb_nevents++;
				if (cb_keys[i].id != 1 || cb_power_post) {
					redwood_touch_post(pressed ? 4 : 5, cb_keys[i].id, 0, 0);
				}
				printf("CUBS buttons: key %u %s\n", cb_keys[i].id, pressed ? "down" : "up");
			}
		}
		cb_pending[i] = pressed;
	}
	/* power + volume down held: back to Android */
	if (cb_hold_ms && cb_state[0] && cb_state[2]) {
		uint64_t now = mach_absolute_time(), elapsed_ns = 0;
		if (cb_hold_start == 0) {
			cb_hold_start = now;
		} else {
			absolutetime_to_nanoseconds(now - cb_hold_start, &elapsed_ns);
		}
		if (elapsed_ns >= (uint64_t)cb_hold_ms * NSEC_PER_MSEC) {
			printf("CUBS buttons: power + volume down held, returning to Android\n");
			cb_hold_start = 0;
			cubs_return_to_android();
		}
	} else {
		cb_hold_start = 0;
	}
}

/*
 * The wall clock: XNU has no RTC here and starts in 1970, where no certificate is valid yet. ios_start.sh writes "TIME<seconds since 1970>" into the shared RAM
 * (PA 0xab2000000 + 0xFF1000) from Android's clock before it starts the servers; this reads it once, a few seconds after boot.
 */
static void
cubs_set_time(void)
{
	volatile uint8_t *t = (volatile uint8_t *)io_map(0xab2000000ULL + 0xFF1000ULL, 0x1000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
	uint64_t secs = 0;
	uint32_t i;

	if (t == NULL) {
		return;
	}
	for (i = 0; i < 40; i++) {
		FlushPoC_DcacheRegion((vm_offset_t)t, 64);
		if (t[0] == 'T' && t[1] == 'I' && t[2] == 'M' && t[3] == 'E' && t[4] >= '0' && t[4] <= '9') {
			break;
		}
		delay_for_interval(500, 1000 * 1000);
	}
	if (i == 40) {
		printf("CUBS time: not given: %02x %02x %02x %02x %02x %02x %02x %02x\n", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
		return;
	}
	for (i = 4; i < 20 && t[i] >= '0' && t[i] <= '9'; i++) {
		secs = secs * 10 + (t[i] - '0');
	}
	if (secs > 1700000000ULL) {
		clock_set_calendar_microtime((clock_sec_t)secs, 0);
		printf("CUBS time: wall clock set to %llu\n", secs);
	}
}

static void
cubs_touch_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t last = 0, ticks = 0;

	cubs_set_time();
		ct_spi = (volatile uint32_t *)ml_io_map(0xda2c000ULL, 0x1000);
	ct_cs = (volatile uint32_t *)ml_io_map(0xe517000ULL, 0x1000);
	ct_irq = (volatile uint32_t *)ml_io_map(0xe536000ULL, 0x1000);
	if (ct_spi == NULL || ct_cs == NULL || ct_irq == NULL) {
		printf("CUBS touch: map failed\n");
		return;
	}
	printf("CUBS touch: start, ssi version 0x%x ctrlr0 0x%x enabled %u cs reg 0x%x irq level %u\n", ct_r(0x5c), ct_r(0),
	    ct_r(CT_SSIENR), ct_cs[0x08 / 4], ct_irq[0x1c / 4] & 1);
	cb_setup();
	for (;;) {
		cb_poll();
		if ((ct_irq[0x1c / 4] & 1) == 0) {
			ct_poll_once();
		} else {
			ct_sleep_ms(2);
		}
		if ((++ticks % 500) == 0) {
			if (ct_mode >= 2 || ct_nframes != last) {
				printf("CUBS touch: nfr 0x%x touches 0x%x backlog 0x%x read 0x%x\n", ct_nframes, ct_ntouch, redwood_touch_backlog(), redwood_touch_consumed() + 0x100);
			}
			last = ct_nframes;
		}
	}
}

static void
cubs_touch_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t th;

	kernel_thread_start(cubs_touch_thread, NULL, &th);
	thread_deallocate(th);
}

__startup_func
static void
cubs_touch_init(void)
{
	uint64_t deadline;
	thread_call_t call;

	if (!PE_parse_boot_argn("cubs_touch", &ct_mode, sizeof(ct_mode)) || ct_mode == 0) {
		return;
	}
	(void)PE_parse_boot_argn("cubs_touch_div", &ct_div, sizeof(ct_div));
	(void)PE_parse_boot_argn("cubs_power_post", &cb_power_post, sizeof(cb_power_post));
	(void)PE_parse_boot_argn("cubs_bye_after", &cb_bye_after, sizeof(cb_bye_after));
	(void)PE_parse_boot_argn("cubs_threads_after", &cb_threads_after, sizeof(cb_threads_after));
	(void)PE_parse_boot_argn("cubs_selfpress", &cb_selfpress, sizeof(cb_selfpress));
	(void)PE_parse_boot_argn("cubs_btn_hold", &cb_hold_ms, sizeof(cb_hold_ms));
	call = thread_call_allocate(cubs_touch_start, NULL);
	clock_interval_to_deadline(8, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_touch_init);
