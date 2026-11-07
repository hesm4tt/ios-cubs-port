
/*
 * CUBS display (boot-arg cubs_blit=1|2): the panel is still lit and scanned by the DPU with the page list of a linear
 * framebuffer that Linux's DRM driver set up before the hand-over (tools drmfb). The list sits in the spare tail of the
 * framebuffer carve (video base + 0xA00000): header {magic, nruns, w, h, pitch, bpp, size, 0}, then {pa, len} runs.
 * cubs_blit=1 copies the RAM framebuffer (boot_args.Video) into those pages ten times a second;
 * cubs_blit=2 draws a colour-cycling test pattern into them instead (proves the path without iOS drawing anything).
 */
#define CUBS_BLIT_MAGIC 0x3130424643425543ULL
#define CUBS_BLIT_MAXRUNS 1024
struct cubs_blit_hdr { uint64_t magic, nruns, w, h, pitch, bpp, size, rsv; };
struct cubs_blit_run { uint64_t pa, len; };
static struct { volatile uint8_t *va; uint64_t start, len; } cubs_blit_run_map[CUBS_BLIT_MAXRUNS];
static uint32_t cubs_blit_nruns, cubs_blit_mode, cubs_blit_ready, cubs_blit_dead;
static uint64_t cubs_blit_pitch, cubs_blit_w, cubs_blit_h, cubs_blit_frames, cubs_blit_nz;
static volatile uint8_t *cubs_blit_src, *cubs_blit_dpu;
static uint32_t cubs_blit_commit;
static thread_call_t cubs_blit_call;

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
		cubs_blit_src = (volatile uint8_t *)ml_io_map_wcomb(PE_state.video.v_baseAddr,
		    (uint64_t)PE_state.video.v_rowBytes * PE_state.video.v_height);
		if (cubs_blit_src == NULL) {
			printf("CUBS blit: source map failed\n");
			cubs_blit_dead = 1;
			return;
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
	if (cubs_blit_mode == 2) {
		cubs_blit_fill(col[(cubs_blit_frames / 20) & 3]);
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
	if (cubs_blit_commit) {
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
	}
	if ((++cubs_blit_frames % 50) == 1) {
		printf("CUBS blit: frame %llu\n", cubs_blit_frames);
		printf("CUBS blit: nz 0x%llx\n", cubs_blit_nz);
	}
	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
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
	cubs_blit_call = thread_call_allocate(cubs_blit_tick, NULL);
	clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(cubs_blit_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_blit_init);
