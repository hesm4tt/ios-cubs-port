/*
 * camsrv: receives the camera frames of the app com.cubs.cam on 127.0.0.1:7777 (header: "CAM1", seq, width, height, length; then I420 planes), keeps the newest
 * in /data/local/tmp/cam_last.i420 (written every CAMSRV_SAVE-th frame, default 30) and, with CAMSRV_RING=1, in the ring for iOS
 * (/dev/cubs_ram, offset 0x1100000: u32 magic "CAMR", u32 seq, u32 width, u32 height, u32 length, then the planes at +0x1000).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RING_OFF 0x1000000UL + 0x100000UL

static int rd_all(int fd, void *p, size_t n)
{
	size_t d = 0;

	while (d < n) {
		ssize_t r = read(fd, (char *)p + d, n - d);

		if (r <= 0) {
			return -1;
		}
		d += (size_t)r;
	}
	return 0;
}

/* the newest frame as RAW10 in the sensor's size and Bayer order that iOS asked for (request at +0x800), into the ring */
static uint8_t *raw;
static size_t raw_cap;
static uint32_t out_seq;

static void
to_ring(int ring, const uint8_t *yuv, uint32_t sw, uint32_t sh, uint32_t seq)
{
	uint32_t rq[6];

	if (pread(ring, rq, sizeof rq, RING_OFF + 0x800) != (ssize_t)sizeof rq || rq[0] != 0x51414d43) {
		return;
	}
	uint32_t w = rq[1], h = rq[2], bayer = rq[3] & 3, stride = rq[4];
	size_t need = (size_t)stride * h;

	if (w < 8 || h < 8 || w > 8192 || h > 8192 || (w & 3) || stride < w * 5 / 4 || need + 0x1000 > 0xF00000) {
		return;
	}
	if (raw_cap < need) {
		raw = realloc(raw, need);
		raw_cap = need;
	}
	const uint8_t *Y = yuv, *U = yuv + (size_t)sw * sh, *V = U + (size_t)(sw / 2) * (sh / 2);
	uint32_t red = bayer, blue = 3 - bayer;

	for (uint32_t y = 0; y < h; y++) {
		uint32_t sy = (uint32_t)((uint64_t)y * sh / h);
		uint8_t *row = raw + (size_t)y * stride;

		for (uint32_t x = 0; x < w; x += 4) {
			uint32_t v[4];

			for (int i = 0; i < 4; i++) {
				uint32_t px = x + (uint32_t)i, sx = (uint32_t)((uint64_t)px * sw / w), site = ((y & 1) ? 2u : 0u) + (px & 1);
				int yy = Y[(size_t)sy * sw + sx], uu = U[(size_t)(sy / 2) * (sw / 2) + sx / 2] - 128, vv = V[(size_t)(sy / 2) * (sw / 2) + sx / 2] - 128, c;

				if (site == red) {
					c = yy + ((359 * vv) >> 8);
				} else if (site == blue) {
					c = yy + ((454 * uu) >> 8);
				} else {
					c = yy - ((88 * uu + 183 * vv) >> 8);
				}
				c = c < 0 ? 0 : c > 255 ? 255 : c;
				v[i] = 64 + (uint32_t)c * 959 / 255;
			}
			uint8_t *g = row + (x / 4) * 5;

			g[0] = (uint8_t)(v[0] >> 2);
			g[1] = (uint8_t)(v[1] >> 2);
			g[2] = (uint8_t)(v[2] >> 2);
			g[3] = (uint8_t)(v[3] >> 2);
			g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
		}
	}
	uint32_t hd[5] = { 0x52414d43, ++out_seq, w, h, (uint32_t)need };

	(void)seq;
	(void)pwrite(ring, raw, need, RING_OFF + 0x1000);
	(void)pwrite(ring, hd, 20, RING_OFF);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7777), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int save = getenv("CAMSRV_SAVE") ? atoi(getenv("CAMSRV_SAVE")) : 30, ring = getenv("CAMSRV_RING") ? open("/dev/cubs_ram", O_RDWR) : -1;
	uint8_t *buf = malloc(8 << 20);

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
		perror("camsrv: bind");
		return 1;
	}
	printf("camsrv: listening on 127.0.0.1:7777%s\n", ring >= 0 ? ", ring on" : "");
	for (;;) {
		int c = accept(ls, NULL, NULL);
		uint32_t h[5];
		unsigned long n = 0;
		time_t t0 = time(NULL);

		printf("camsrv: app connected\n");
		while (rd_all(c, h, 20) == 0 && h[0] == 0x31414d43 && h[4] <= (8u << 20) && rd_all(c, buf, h[4]) == 0) {
			n++;
			if (save && (n % (unsigned)save) == 1) {
				int f = open("/data/local/tmp/cam_last.i420", O_WRONLY | O_CREAT | O_TRUNC, 0644);

				if (f >= 0) {
					(void)write(f, buf, h[4]);
					close(f);
				}
			}
			if (ring >= 0) {
				to_ring(ring, buf, h[2], h[3], h[1]);
			}
			if ((n % 60) == 0) {
				long dt = (long)(time(NULL) - t0);

				printf("camsrv: %lu frames %ux%u, %.1f fps\n", n, h[2], h[3], dt ? (double)n / (double)dt : 0.0);
			}
		}
		printf("camsrv: app gone after %lu frames\n", n);
		close(c);
	}
}
