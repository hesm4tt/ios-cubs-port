/*
 * gpuc: an iOS userland client of /dev/cubs-gpu (see redwood_gpu.c). It asks the Linux GPU server (pcwork/gpusrv, the
 * metal2vulkan runtime on the Pixel's PowerVR) for frames of a rotating triangle, through the ring in reserved RAM, and
 * prints what it gets back: the first proof that an iOS *process* renders through the Linux-side GPU.
 * Dynamically linked, built like userland/src/rwtouch.c; run by a launchd job list line "/redwood/bin/gpuc".
 * Ring layout: pcwork/gpusrv/src/ring.rs (u32 words: 0 magic, 1 request, 2 done, 3 w, 4 h, 5 vertices, 6 clear, 7 us;
 * vertices x y r g b a (f32) at 64, RGBA8 pixels at 65536).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define W 256
#define H 512

static int fd;

static uint32_t
rd32(off_t off)
{
	uint32_t v = 0;

	if (pread(fd, &v, 4, off) != 4) {
		return 0;
	}
	return v;
}

static void
wr32(off_t off, uint32_t v)
{
	(void)pwrite(fd, &v, 4, off);
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int
main(void)
{
	static uint32_t px[W * H];
	double t0, tlast;
	unsigned frames = 0, step = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int tries = 0;; tries++) {
		fd = open("/dev/cubs-gpu", O_RDWR);
		if (fd >= 0 && rd32(0) == 0x47505531u) {
			break;
		}
		if (fd >= 0) {
			close(fd);
		}
		if (tries % 30 == 0) {
			printf("gpuc: waiting for /dev/cubs-gpu and the GPU server (try %d): fd %d errno %d magic %08x\n", tries, fd,
			    fd < 0 ? errno : 0, fd >= 0 ? rd32(0) : 0);
		}
		sleep(2);
	}
	printf("gpuc: connected to the GPU server\n");
	wr32(3 * 4, W);
	wr32(4 * 4, H);
	wr32(6 * 4, 0xff402010);
	t0 = tlast = now();
	for (;;) {
		float v[18];
		double a = step++ * 0.07;
		static const float tri[3][5] = { { 0.0f, -0.6f, 1, 0, 0 }, { 0.6f, 0.6f, 0, 1, 0 }, { -0.6f, 0.6f, 0, 0, 1 } };

		for (int i = 0; i < 3; i++) {
			float x = tri[i][0], y = tri[i][1];

			v[i * 6 + 0] = (float)(x * cos(a) - y * sin(a));
			v[i * 6 + 1] = (float)((x * sin(a) + y * cos(a)) * ((double)W / H));
			v[i * 6 + 2] = tri[i][2];
			v[i * 6 + 3] = tri[i][3];
			v[i * 6 + 4] = tri[i][4];
			v[i * 6 + 5] = 1.0f;
		}
		(void)pwrite(fd, v, sizeof v, 64);
		wr32(5 * 4, 3);
		uint32_t seq = rd32(1 * 4) + 1;

		wr32(1 * 4, seq);
		int spins = 0;

		while (rd32(2 * 4) != seq) {
			if (++spins > 2000) {
				break;
			}
			usleep(2000);
		}
		if (rd32(2 * 4) != seq) {
			printf("gpuc: the server did not answer frame %u\n", seq);
			continue;
		}
		(void)pread(fd, px, sizeof px, 65536);
		frames++;
		if (frames % 25 == 1) {
			double t = now();
			uint32_t centre = px[(H / 2) * W + W / 2], corner = px[0];

			printf("gpuc: frame %u, server %u us, %.1f fps, centre %08x corner %08x\n", frames, rd32(7 * 4),
			    25.0 / (t - tlast), centre, corner);
			tlast = t;
		}
		usleep(40000);
		(void)t0;
	}
}
