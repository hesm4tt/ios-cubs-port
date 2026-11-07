
/*
 * CUBS GPU client (boot-arg cubs_gpu=1): Linux runs the GPU server (pcwork/gpusrv, metal2vulkan on the PowerVR) beside
 * XNU; the two talk through a ring in reserved RAM that XNU does not manage (PA cubs_gpu_pa, boot-arg cubs_gpu_pa=<hex>,
 * default 0xab2000000). This is the proof of concept of the Metal route: every 40 ms XNU asks the server for a frame of a
 * rotating triangle and pastes the pixels it gets back over the iOS display (540x1212 at 270,600).
 * Ring layout (see gpusrv/src/ring.rs): word 0 magic "GPU1", 1 request counter, 2 done counter, 3 w, 4 h, 5 vertex count,
 * 6 clear colour, 7 server microseconds; vertices (x y r g b a, f32) at +64; RGBA8 pixels at +65536.
 */
#define CUBS_GPU_MAGIC 0x47505531u
#define CUBS_GPU_W 360u
#define CUBS_GPU_H 808u
static uint64_t cubs_gpu_pa = 0xab2000000ULL;
static volatile uint8_t *cubs_gpu_ring;
static thread_call_t cubs_gpu_call;
static uint32_t cubs_gpu_wait, cubs_gpu_step, cubs_gpu_seq, cubs_gpu_frames, cubs_gpu_bad, cubs_gpu_dead;

static void
cubs_gpu_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	volatile uint32_t *w;
	uint64_t deadline;

	if (cubs_gpu_dead) {
		return;
	}
	if (cubs_gpu_ring == NULL) {
		if (!cubs_blit_ready) {
			clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
			thread_call_enter_delayed(cubs_gpu_call, deadline);
			return;
		}
		cubs_gpu_ring = (volatile uint8_t *)io_map(cubs_gpu_pa, 0x1000000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
		if (cubs_gpu_ring == NULL) {
			printf("CUBS gpu: map of the ring at %llx failed\n", cubs_gpu_pa);
			cubs_gpu_dead = 1;
			return;
		}
		printf("CUBS gpu: ring mapped at %llx\n", cubs_gpu_pa);
	}
	w = (volatile uint32_t *)cubs_gpu_ring;
	FlushPoC_DcacheRegion((vm_offset_t)cubs_gpu_ring, 64);
	if (w[0] != CUBS_GPU_MAGIC) {
		if ((cubs_gpu_bad++ % 20) == 0) {
			printf("CUBS gpu: no server yet (magic %x)\n", w[0]);
		}
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cubs_gpu_call, deadline);
		return;
	}
	if (!cubs_gpu_wait) {
		volatile uint32_t *v = (volatile uint32_t *)(cubs_gpu_ring + 64);
		const uint32_t *t = &cubs_gpu_tab[(cubs_gpu_step++ & 63) * 18];

		for (uint32_t i = 0; i < 18; i++) {
			v[i] = t[i];
		}
		w[3] = CUBS_GPU_W;
		w[4] = CUBS_GPU_H;
		w[5] = 3;
		w[6] = 0xff1a0d0d;
		__asm__ volatile ("dsb sy" ::: "memory");
		FlushPoC_DcacheRegion((vm_offset_t)cubs_gpu_ring, 64 + 18 * 4);
		cubs_gpu_seq = w[1] + 1;
		w[1] = cubs_gpu_seq;
		__asm__ volatile ("dsb sy" ::: "memory");
		FlushPoC_DcacheRegion((vm_offset_t)cubs_gpu_ring, 64);
		cubs_gpu_wait = 1;
		clock_interval_to_deadline(4, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cubs_gpu_call, deadline);
		return;
	}
	FlushPoC_DcacheRegion((vm_offset_t)cubs_gpu_ring, 64);
	if (w[2] != cubs_gpu_seq) {
		if (++cubs_gpu_wait > 200) {    /* the server went away: ask again */
			cubs_gpu_wait = 0;
		}
		clock_interval_to_deadline(2, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cubs_gpu_call, deadline);
		return;
	}
	cubs_gpu_wait = 0;
	{
		static uint64_t row[CUBS_GPU_W / 2];
		uint32_t cur = 0, ox = 0, oy = 0;
		const uint32_t *src;

		FlushPoC_DcacheRegion((vm_offset_t)(cubs_gpu_ring + 65536), CUBS_GPU_W * CUBS_GPU_H * 4);
		for (uint32_t y = 0; y < CUBS_GPU_H; y++) {
			src = (const uint32_t *)(cubs_gpu_ring + 65536 + (uint64_t)y * CUBS_GPU_W * 4);
			for (uint32_t x = 0; x < CUBS_GPU_W; x += 2) {
				uint32_t a = src[x], b = src[x + 1];

				a = (a & 0xff00ff00u) | ((a & 0xff) << 16) | ((a >> 16) & 0xff);   /* RGBA -> BGRA */
				b = (b & 0xff00ff00u) | ((b & 0xff) << 16) | ((b >> 16) & 0xff);
				row[x / 2] = ((uint64_t)b << 32) | a;
			}
			cubs_blit_put(&cur, (uint64_t)(oy + y) * cubs_blit_pitch + (uint64_t)ox * 4, row, CUBS_GPU_W * 4);
		}
	}
	cubs_gpu_dirty = 1;
	cubs_blit_frame_done();
	if ((++cubs_gpu_frames % 50) == 1) {
		printf("CUBS gpu: frame %u, server %u us\n", cubs_gpu_frames, w[7]);
	}
	clock_interval_to_deadline(80, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(cubs_gpu_call, deadline);
}

__startup_func
static void
cubs_gpu_init(void)
{
	uint64_t deadline, pa;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("cubs_gpu", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (PE_parse_boot_argn("cubs_gpu_pa", &pa, sizeof(pa)) && pa != 0) {
		cubs_gpu_pa = pa;
	}
	cubs_gpu_call = thread_call_allocate(cubs_gpu_tick, NULL);
	clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(cubs_gpu_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_gpu_init);

/*
 * CUBS present (boot-arg cubs_present=1): the Metal RPC server (mtlsrv, op 11 PRESENT) leaves a rendered texture's RGBA8 pixels
 * at ring + 65536 and bumps a present counter in the Metal mailbox (ring + 0xB00000, words 8 counter, 9 w, 10 h, 11 x, 12 y).
 * This thread pastes them into the scan-out pages whenever the counter moves: what an iOS program renders through libmtlshim
 * and presents appears on the panel beside what iOS itself draws.
 */
static thread_call_t cubs_present_call;
static uint32_t cubs_present_last, cubs_present_frames;
static volatile uint8_t *cubs_present_ring;

static void
cubs_present_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static uint64_t row[540];
	volatile uint32_t *m;
	uint64_t deadline;

	if (cubs_present_ring == NULL) {
		if (cubs_blit_ready) {
			cubs_present_ring = (volatile uint8_t *)io_map(cubs_gpu_pa, 0x1000000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
			printf("CUBS present: ring %p\n", (void *)cubs_present_ring);
		}
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cubs_present_call, deadline);
		return;
	}
	m = (volatile uint32_t *)(cubs_present_ring + 0xB00000);
	FlushPoC_DcacheRegion((vm_offset_t)m, 64);
	if (m[0] == 0x4d544c31 && m[8] != cubs_present_last) {
		uint32_t w = m[9], h = m[10], ox = m[11], oy = m[12], cur = 0;

		if (w == 0 || w > 1080 || h == 0 || h > 2424 || (w & 1)) {
			cubs_present_last = m[8];
		} else {
			FlushPoC_DcacheRegion((vm_offset_t)(cubs_present_ring + 65536), (unsigned)(w * h * 4));
			for (uint32_t y = 0; y < h; y++) {
				const uint32_t *src = (const uint32_t *)(cubs_present_ring + 65536 + (uint64_t)y * w * 4);

				for (uint32_t x = 0; x < w; x += 2) {
					uint32_t a = src[x], b = src[x + 1];

					a = (a & 0xff00ff00u) | ((a & 0xff) << 16) | ((a >> 16) & 0xff);   /* RGBA -> BGRA */
					b = (b & 0xff00ff00u) | ((b & 0xff) << 16) | ((b >> 16) & 0xff);
					row[x / 2] = ((uint64_t)b << 32) | a;
				}
				cubs_blit_put(&cur, (uint64_t)(oy + y) * cubs_blit_pitch + (uint64_t)ox * 4, row, w * 4);
			}
			cubs_present_last = m[8];
			cubs_gpu_dirty = 1;
			cubs_blit_frame_done();
			if ((++cubs_present_frames % 20) == 1) {
				printf("CUBS present: frame %u (%ux%u at %u,%u)\n", cubs_present_frames, w, h, ox, oy);
			}
		}
	}
	clock_interval_to_deadline(25, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(cubs_present_call, deadline);
}

__startup_func
static void
cubs_present_init(void)
{
	uint64_t deadline, pa;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("cubs_present", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (PE_parse_boot_argn("cubs_gpu_pa", &pa, sizeof(pa)) && pa != 0) {
		cubs_gpu_pa = pa;
	}
	cubs_present_call = thread_call_allocate(cubs_present_tick, NULL);
	clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(cubs_present_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cubs_present_init);
