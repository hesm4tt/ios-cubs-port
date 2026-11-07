extern unsigned int processor_avail_count;

#include <vm/vm_kern.h>
#include <arm/caches_internal.h>

/*
 * CUBS display (boot-arg cubs_blit=1|2): the panel is still lit and scanned by the DPU with the page list of a linear
 * framebuffer that Linux's DRM driver set up before the hand-over (tools drmfb). The list sits in the spare tail of the
 * framebuffer carve (video base + 0xA00000): header {magic, nruns, w, h, pitch, bpp, size, 0}, then {pa, len} runs.
 * cubs_blit=1 copies the RAM framebuffer (boot_args.Video) into those pages ten times a second;
 * cubs_blit=2 draws a colour-cycling test pattern into them instead (proves the path without iOS drawing anything).
 */
#define CUBS_BLIT_MAGIC 0x3130424643425543ULL
#define CUBS_BLIT_MAXRUNS 2688     /* a 10.5 MB buffer is at most 2576 pages, and the list page holds 4092 runs */
struct cubs_blit_hdr { uint64_t magic, nruns, w, h, pitch, bpp, size, rsv; };
struct cubs_blit_run { uint64_t pa, len; };
static struct { volatile uint8_t *va; uint64_t start, len; } cubs_blit_run_map[CUBS_BLIT_MAXRUNS];
static uint32_t cubs_blit_nruns, cubs_blit_mode, cubs_blit_ready, cubs_blit_dead;
static uint64_t cubs_blit_pitch, cubs_blit_w, cubs_blit_h, cubs_blit_frames, cubs_blit_nz;
static volatile uint8_t *cubs_blit_src, *cubs_blit_dpu;
static uint32_t cubs_blit_commit;
static thread_call_t cubs_blit_call;
/* fast path (boot-arg cubs_blit_fast, default 1): cacheable source + shadow compare, only changed 16-row bands are
 * written to the scan-out pages and the DPU commit runs only when something changed */
static uint32_t cubs_blit_fast = 1, cubs_blit_ms, cubs_blit_dirty;
static volatile uint8_t *cubs_blit_srcc;
static uint8_t *cubs_blit_shadow;
static uint64_t cubs_blit_cost_us, cubs_blit_cost_n;
/* tear avoidance (boot-arg cubs_blit_hold, ms, default 20, 0 = off): the DPU reads the scan-out pages for a while after each commit, so the next band writes
 * wait until that read-out has had time to finish; writes then never overlap a frame in flight */
static uint32_t cubs_blit_hold = 20;
static uint64_t cubs_blit_last_commit, cubs_blit_holds;
#define CUBS_BLIT_BAND 16

static void
cubs_blit_setup(void)
{
	uint64_t lpa = PE_state.video.v_baseAddr + 0xA00000ULL, off = 0;
	volatile struct cubs_blit_hdr *h = (volatile struct cubs_blit_hdr *)ml_io_map_wcomb(lpa, 0x10000);
	volatile struct cubs_blit_run *r;
	uint32_t i;

	if (h == NULL || h->magic != CUBS_BLIT_MAGIC || h->nruns == 0 || h->nruns > CUBS_BLIT_MAXRUNS || h->bpp != 32) {
		printf("CUBS blit: no valid page list at %llx (magic %llx runs %llu)\n", lpa, h ? h->magic : 0, h ? h->nruns : 0);
		cubs_blit_dead = 1;
		return;
	}
	r = (volatile struct cubs_blit_run *)((volatile uint8_t *)h + sizeof(*h));
	cubs_blit_nruns = (uint32_t)h->nruns;
	cubs_blit_w = h->w; cubs_blit_h = h->h; cubs_blit_pitch = h->pitch;
	for (i = 0; i < cubs_blit_nruns; i++) {
		cubs_blit_run_map[i].va = (volatile uint8_t *)ml_io_map_wcomb(r[i].pa, r[i].len);
		cubs_blit_run_map[i].start = off;
		cubs_blit_run_map[i].len = r[i].len;
		off += r[i].len;
		if (cubs_blit_run_map[i].va == NULL) {
			printf("CUBS blit: map of run %u failed\n", i);
			cubs_blit_dead = 1;
			return;
		}
	}
	if (cubs_blit_mode == 1) {
		uint64_t fbsize = (uint64_t)PE_state.video.v_rowBytes * PE_state.video.v_height;

		cubs_blit_src = (volatile uint8_t *)ml_io_map_wcomb(PE_state.video.v_baseAddr, fbsize);
		if (cubs_blit_src == NULL) {
			printf("CUBS blit: source map failed\n");
			cubs_blit_dead = 1;
			return;
		}
		if (cubs_blit_fast) {
			vm_offset_t sh = 0;

			cubs_blit_srcc = (volatile uint8_t *)io_map(PE_state.video.v_baseAddr, fbsize, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
			kmem_alloc(kernel_map, &sh, round_page(fbsize), KMA_NOFAIL | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_DIAG);
			cubs_blit_shadow = (uint8_t *)sh;
			if (cubs_blit_srcc == NULL || cubs_blit_shadow == NULL) {
				printf("CUBS blit: fast path unavailable, using the plain copy\n");
				cubs_blit_fast = 0;
			}
		}
	}
	if (cubs_blit_commit) {
		cubs_blit_dpu = (volatile uint8_t *)ml_io_map(0xf600000ULL, 0x120000);
		if (cubs_blit_dpu == NULL) {
			cubs_blit_dead = 1;
			return;
		}
		printf("CUBS blit: dpu 0x%x\n", ((volatile uint32_t *)cubs_blit_dpu)[0x80004 / 4]);
	}
	printf("CUBS blit: %u runs, %llux%llu pitch %llu, mode %u ready\n", cubs_blit_nruns, cubs_blit_w, cubs_blit_h,
	    cubs_blit_pitch, cubs_blit_mode);
	cubs_blit_ready = 1;
}

/* copy n bytes (multiples of 8) to offset doff of the scan-out buffer; *cur is the run to start looking from */
static void
cubs_blit_put(uint32_t *cur, uint64_t doff, const volatile uint64_t *s, uint64_t n)
{
	while (n && *cur < cubs_blit_nruns) {
		uint64_t st = cubs_blit_run_map[*cur].start, ln = cubs_blit_run_map[*cur].len;
		if (doff >= st + ln) {
			(*cur)++;
			continue;
		}
		uint64_t c = st + ln - doff;
		if (c > n) {
			c = n;
		}
		volatile uint64_t *d = (volatile uint64_t *)(cubs_blit_run_map[*cur].va + (doff - st));
		for (uint64_t i = 0; i < c / 8; i++) {
			d[i] = s[i];
		}
		doff += c; n -= c; s += c / 8;
	}
}

static void
cubs_blit_fill(uint32_t color)
{
	for (uint32_t i = 0; i < cubs_blit_nruns; i++) {
		volatile uint64_t *d = (volatile uint64_t *)cubs_blit_run_map[i].va;
		uint64_t v = ((uint64_t)color << 32) | color;
		for (uint64_t k = 0; k < cubs_blit_run_map[i].len / 8; k++) {
			d[k] = v;
		}
	}
}

/*
 * Event-driven blitting: MiniIOMobileFramebuffer::show() calls cubs_blit_frame_done() when a swap has been composited
 * into the boot framebuffer. The blit then runs a couple of milliseconds later; between frames nothing is polled except
 * a slow safety pass (a frame drawn by the kernel console, before iOS draws, does not announce itself).
 */
static volatile uint32_t cubs_blit_pending, cubs_blit_events;
static volatile uint32_t cubs_gpu_dirty;        /* the GPU client pasted pixels: the DPU commit must run (cubs_gpu.c.inc) */

void
cubs_blit_frame_done(void)
{
	uint64_t deadline;

	cubs_blit_events++;
	cubs_blit_pending = 1;
	if (cubs_blit_call != NULL && cubs_blit_ready) {
		clock_interval_to_deadline(2, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cubs_blit_call, deadline);
	}
}

/* boot-arg cubs_shot=1: every ~5 s a quarter-size copy of the iOS framebuffer goes into the shared RAM (PA 0xab2000000 + 0x1C00000: "SHOT", sequence, width, height, then BGRA) for the host to read */
static uint32_t cubs_shot_on, cubs_shot_seq;
static volatile uint8_t *cubs_shot_ring;

static void
cubs_shot_take(void)
{
	uint32_t W, H, y, x;
	uint64_t pitch = PE_state.video.v_rowBytes;
	volatile uint32_t *dst;

	if (cubs_blit_srcc == NULL) {
		return;
	}
	if (cubs_shot_ring == NULL) {
		cubs_shot_ring = (volatile uint8_t *)io_map(0xab2000000ULL + 0x1C00000ULL, 0x400000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
		if (cubs_shot_ring == NULL) {
			return;
		}
	}
	W = 540;
	H = 1212;
	dst = (volatile uint32_t *)(cubs_shot_ring + 0x1000);
	for (y = 0; y < H; y++) {
		const volatile uint32_t *row = (const volatile uint32_t *)(cubs_blit_srcc + (uint64_t)(2 * y) * pitch);

		for (x = 0; x < W; x++) {
			dst[y * W + x] = row[2 * x];
		}
	}
	((volatile uint32_t *)cubs_shot_ring)[1] = ++cubs_shot_seq;
	((volatile uint32_t *)cubs_shot_ring)[2] = W;
	((volatile uint32_t *)cubs_shot_ring)[3] = H;
	__asm__ volatile ("dsb sy" ::: "memory");
	((volatile uint32_t *)cubs_shot_ring)[0] = 0x544f4853;
	FlushPoC_DcacheRegion((vm_offset_t)cubs_shot_ring, 0x1000 + W * H * 4);
	printf("CUBS shot: %u taken\n", cubs_shot_seq);
}

static void
cubs_blit_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const uint32_t col[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
	uint64_t deadline;

	if (cubs_blit_dead) {
		return;
	}
	if (!cubs_blit_ready) {
		cubs_blit_setup();
		if (cubs_blit_dead) {
			return;
		}
	}
	if (cubs_blit_mode == 1 && cubs_blit_fast && cubs_blit_events != 0 && !cubs_blit_pending) {
		static uint32_t idle;

		if ((++idle & 3) != 0) {        /* safety pass only every 4th slow tick */
			clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
			thread_call_enter_delayed(cubs_blit_call, deadline);
			return;
		}
	}
	if (cubs_blit_commit && cubs_blit_hold && cubs_blit_last_commit) {
		uint64_t ns, hold_ns = (uint64_t)cubs_blit_hold * NSEC_PER_MSEC;

		absolutetime_to_nanoseconds(mach_absolute_time() - cubs_blit_last_commit, &ns);
		if (ns < hold_ns) {
			cubs_blit_holds++;
			clock_interval_to_deadline((hold_ns - ns) / 1000 + 1, NSEC_PER_USEC, &deadline);
			thread_call_enter_delayed(cubs_blit_call, deadline);
			return;
		}
	}
	cubs_blit_pending = 0;
	uint64_t t0 = mach_absolute_time();

	cubs_blit_dirty = 1;
	if (cubs_blit_mode == 2) {
		cubs_blit_fill(col[(cubs_blit_frames / 20) & 3]);
	} else if (cubs_blit_fast) {
		uint32_t cur = 0;
		uint64_t rb = PE_state.video.v_rowBytes;

		cubs_blit_dirty = 0;
		cubs_blit_nz = 0;
		FlushPoC_DcacheRegion((vm_offset_t)cubs_blit_srcc, rb * cubs_blit_h);
		for (uint64_t y0 = 0; y0 < cubs_blit_h; y0 += CUBS_BLIT_BAND) {
			uint64_t rows = cubs_blit_h - y0 < CUBS_BLIT_BAND ? cubs_blit_h - y0 : CUBS_BLIT_BAND;

			if (memcmp((const void *)(cubs_blit_srcc + y0 * rb), cubs_blit_shadow + y0 * rb, rows * rb) != 0) {
				memcpy(cubs_blit_shadow + y0 * rb, (const void *)(cubs_blit_srcc + y0 * rb), rows * rb);
				for (uint64_t y = y0; y < y0 + rows; y++) {
					cubs_blit_put(&cur, y * cubs_blit_pitch, (const volatile uint64_t *)(cubs_blit_shadow + y * rb), rb);
				}
				cubs_blit_dirty = 1;
				cubs_blit_nz++;     /* bands written */
			}
		}
	} else {
		uint32_t cur = 0;
		uint64_t rb = PE_state.video.v_rowBytes;
		cubs_blit_nz = 0;
		for (uint64_t y = 0; y < cubs_blit_h; y++) {
			const volatile uint64_t *row = (const volatile uint64_t *)(cubs_blit_src + y * rb);
			if ((y & 7) == 0) {
				for (uint64_t k = 0; k < rb / 8; k++) {
					if (row[k] != *(const volatile uint64_t *)cubs_blit_src) {
						cubs_blit_nz++;
					}
				}
			}
			cubs_blit_put(&cur, y * cubs_blit_pitch, row, rb);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (cubs_gpu_dirty) {
		cubs_gpu_dirty = 0;
		cubs_blit_dirty = 1;
	}
	if (cubs_blit_commit && cubs_blit_dirty) {
		/* Linux's per-frame DPU commit sequence (captured in the wake trace), planes at 0xf618000 + 0x4000 * k */
#define CBW(a, v) (*(volatile uint32_t *)(cubs_blit_dpu + ((uint64_t)(a) - 0xf600000ULL)) = (v))
		CBW(0xf680008ULL, 2);
		CBW(0xf680008ULL, 0x2000000);
		CBW(0xf680008ULL, 0x4000000);
		for (uint32_t kk = 0; kk < 6; kk++) {
			CBW(0xf6d7000ULL + 0x1000ULL * kk, 0);
		}
		for (uint32_t kk = 0; kk < 6; kk++) {
			CBW(0xf618000ULL + 0x4000ULL * kk, 1);
		}
		CBW(0xf68c000ULL, 1);
		CBW(0xf6b606cULL, 1);
		CBW(0xf680008ULL, 1);
		CBW(0xf6d4004ULL, 0x4320);
		CBW(0xf6b6000ULL, 1);
		CBW(0xf6b6004ULL, 1);
		CBW(0xf680008ULL, 0x2000001);
		CBW(0xf680008ULL, 0x4000000);
		CBW(0xf6000f0ULL, 0x400);
		CBW(0xf6000ecULL, 0xffffff);
		CBW(0xf6400ecULL, 0xffffff);
		for (uint32_t kk = 0; kk < 6; kk++) {
			CBW(0xf618000ULL + 0x4000ULL * kk, 0);
		}
		CBW(0xf68c000ULL, 0);
		CBW(0xf6b606cULL, 0);
#undef CBW
		__asm__ volatile ("dsb sy; isb" ::: "memory");
		cubs_blit_last_commit = mach_absolute_time();
	}
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		cubs_blit_cost_us += ns / 1000;
		cubs_blit_cost_n++;
	}
	if ((++cubs_blit_frames % 50) == 1) {
		printf("CUBS blit: frame %llu\n", cubs_blit_frames);
		printf("CUBS blit: holds 0x%llx\n", cubs_blit_holds);
		if (cubs_shot_on) {
			cubs_shot_take();
		}
		printf("CUBS blit: processors %u, this one %d\n", (unsigned)processor_avail_count, (int)cpu_number());
		printf("CUBS blit: fb pa %#llx rowbytes %lu height %lu\n", (unsigned long long)PE_state.video.v_baseAddr, (unsigned long)PE_state.video.v_rowBytes, (unsigned long)PE_state.video.v_height);
		printf("CUBS blit: nz 0x%llx\n", cubs_blit_nz);
		printf("CUBS blit: cost 0x%llx\n", cubs_blit_cost_us / (cubs_blit_cost_n ? cubs_blit_cost_n : 1));
		{
			/* probe: non-zero words (every 64th) seen through the cacheable alias, the write-combined alias and the shadow */
			uint64_t fbs = (uint64_t)PE_state.video.v_rowBytes * PE_state.video.v_height, o;
			uint32_t pa = 0, pb = 0, pc = 0;

			for (o = 0; o + 4 <= fbs; o += 256) {
				if (cubs_blit_srcc && *(volatile uint32_t *)(cubs_blit_srcc + o)) {
					pa++;
				}
				if (cubs_blit_src && *(volatile uint32_t *)(cubs_blit_src + o)) {
					pb++;
				}
				if (cubs_blit_shadow && *(uint32_t *)(cubs_blit_shadow + o)) {
					pc++;
				}
			}
			printf("CUBS blit: probe cacheable %u wcomb %u shadow %u fast %u\n", pa, pb, pc, cubs_blit_fast);
			if (cubs_blit_shadow) {
				/* pixels that are really drawn (not 0 / opaque black) in the shadow: are they in the scan-out pages too? */
				uint32_t lit = 0, miss = 0, litsrc = 0;
				uint64_t rb2 = PE_state.video.v_rowBytes, y2, x2;

				for (y2 = 0; y2 < PE_state.video.v_height; y2 += 8) {
					for (x2 = 0; x2 < 1080; x2 += 8) {
						uint32_t sh = *(uint32_t *)(cubs_blit_shadow + y2 * rb2 + x2 * 4);
						uint32_t so = 0xdeadbeef;

						if (cubs_blit_srcc && (*(volatile uint32_t *)(cubs_blit_srcc + y2 * rb2 + x2 * 4) & 0xffffff)) {
							litsrc++;
						}
						if ((sh & 0xffffff) == 0) {
							continue;
						}
						lit++;
						for (uint32_t r = 0; r < cubs_blit_nruns; r++) {
							uint64_t doff = y2 * cubs_blit_pitch + x2 * 4, st = cubs_blit_run_map[r].start;

							if (doff >= st && doff < st + cubs_blit_run_map[r].len) {
								so = *(volatile uint32_t *)(cubs_blit_run_map[r].va + (doff - st));
								break;
							}
						}
						if (so != sh) {
							miss++;
						}
					}
				}
				printf("CUBS blit: lit src %u shadow %u scanout-miss %u\n", litsrc, lit, miss);
			}
		}
		cubs_blit_cost_us = 0;
		cubs_blit_cost_n = 0;
	}
	clock_interval_to_deadline(cubs_blit_pending ? 2 : (cubs_blit_events != 0 && cubs_blit_mode == 1 && cubs_blit_fast ? 500 : cubs_blit_ms), NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(cubs_blit_call, deadline);
}

__startup_func
static void
cubs_blit_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("cubs_blit", &cubs_blit_mode, sizeof(cubs_blit_mode)) || cubs_blit_mode == 0) {
		return;
	}
	if (cubs_blit_mode >= 3) {      /* 3 = iOS framebuffer + per-frame DPU commit, 4 = colour cycle + commit */
		cubs_blit_commit = 1;
		cubs_blit_mode = cubs_blit_mode == 4 ? 2 : 1;
	}
	(void)PE_parse_boot_argn("cubs_blit_fast", &cubs_blit_fast, sizeof(cubs_blit_fast));
	(void)PE_parse_boot_argn("cubs_blit_hold", &cubs_blit_hold, sizeof(cubs_blit_hold));
	(void)PE_parse_boot_argn("cubs_shot", &cubs_shot_on, sizeof(cubs_shot_on));
	if (!PE_parse_boot_argn("cubs_blit_ms", &cubs_blit_ms, sizeof(cubs_blit_ms)) || cubs_blit_ms == 0) {
		cubs_blit_ms = 100;
	}
	cubs_blit_call = thread_call_allocate(cubs_blit_tick, NULL);
	clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(cubs_blit_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_blit_init);
