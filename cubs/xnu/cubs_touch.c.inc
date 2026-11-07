
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

static void
cubs_touch_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t last = 0, ticks = 0;

		ct_spi = (volatile uint32_t *)ml_io_map(0xda2c000ULL, 0x1000);
	ct_cs = (volatile uint32_t *)ml_io_map(0xe517000ULL, 0x1000);
	ct_irq = (volatile uint32_t *)ml_io_map(0xe536000ULL, 0x1000);
	if (ct_spi == NULL || ct_cs == NULL || ct_irq == NULL) {
		printf("CUBS touch: map failed\n");
		return;
	}
	printf("CUBS touch: start, ssi version 0x%x ctrlr0 0x%x enabled %u cs reg 0x%x irq level %u\n", ct_r(0x5c), ct_r(0),
	    ct_r(CT_SSIENR), ct_cs[0x08 / 4], ct_irq[0x1c / 4] & 1);
	for (;;) {
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
	call = thread_call_allocate(cubs_touch_start, NULL);
	clock_interval_to_deadline(8, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_touch_init);
