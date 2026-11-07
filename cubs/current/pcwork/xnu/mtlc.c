/*
 * mtlc: iOS-side client of the Metal RPC (pcwork/gpusrv/src/bin/mtlsrv.rs): runs the metal2vulkan compute_dispatch example
 * from an iOS process through /dev/cubs-gpu -- library from AIR bitcode (fixture_bc.h, generated with xxd -i), pipeline,
 * three buffers, one dispatch, read back -- and checks the result on the CPU.
 * Dynamically linked like userland/src/rwtouch.c; run by a launchd job list line "/redwood/bin/mtlc &".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "fixture_bc.h"

#define RPC_OFF 0xB00000
#define PAYLOAD_OFF (RPC_OFF + 0x1000)

static int fd;
static uint32_t seq;

static uint32_t
word(int i)
{
	uint32_t v = 0;

	(void)pread(fd, &v, 4, RPC_OFF + i * 4);
	return v;
}

static void
setword(int i, uint32_t v)
{
	(void)pwrite(fd, &v, 4, RPC_OFF + i * 4);
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* one call: returns the status; *result the id/value word, resp (if not NULL) filled with *rlen bytes */
static uint32_t
call(uint32_t op, const void *payload, uint32_t plen, uint32_t *result, void *resp, uint32_t *rlen)
{
	if (plen) {
		(void)pwrite(fd, payload, plen, PAYLOAD_OFF);
	}
	setword(4, op);
	setword(6, plen);
	seq++;
	setword(1, seq);
	double t = now();

	while (word(2) != seq) {
		if (now() - t > 30) {
			printf("mtlc: the server did not answer op %u\n", op);
			exit(2);
		}
		usleep(500);
	}
	uint32_t status = word(3), n = word(6);

	if (result) {
		*result = word(5);
	}
	if (resp && n) {
		(void)pread(fd, resp, n, PAYLOAD_OFF);
	}
	if (rlen) {
		*rlen = n;
	}
	return status;
}

static uint32_t
mkbuf(uint32_t len, const void *init)
{
	static uint8_t p[8 + 4096];
	uint32_t id = 0, has = init != NULL;

	memcpy(p, &len, 4);
	memcpy(p + 4, &has, 4);
	if (init) {
		memcpy(p + 8, init, len);
	}
	if (call(2, p, 8 + (init ? len : 0), &id, NULL, NULL) != 0) {
		printf("mtlc: new buffer failed\n");
		exit(3);
	}
	return id;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int tries = 0;; tries++) {
		fd = open("/dev/cubs-gpu", O_RDWR);
		if (fd >= 0 && word(0) == 0x4d544c31u) {
			break;
		}
		if (fd >= 0) {
			close(fd);
		}
		if (tries % 30 == 0) {
			printf("mtlc: waiting for /dev/cubs-gpu and the Metal server (try %d)\n", tries);
		}
		sleep(2);
	}
	seq = word(1);
	printf("mtlc: connected to the Metal server\n");

	uint32_t lib = 0, pso = 0;

	if (call(1, fixture_bc, fixture_bc_len, &lib, NULL, NULL) != 0) {
		printf("mtlc: new library failed\n");
		return 4;
	}
	uint8_t name[64];
	const char *fn = "scale_and_offset";

	memcpy(name, &lib, 4);
	memcpy(name + 4, fn, strlen(fn));
	if (call(5, name, 4 + (uint32_t)strlen(fn), &pso, NULL, NULL) != 0) {
		printf("mtlc: new compute pipeline failed\n");
		return 5;
	}
	printf("mtlc: library %u pipeline %u, max threads %u\n", lib, pso, word(7));

	enum { N = 1000 };
	static float input[N], output[N];
	float scale = 2.0f;

	for (int i = 0; i < N; i++) {
		input[i] = (float)i;
	}
	/* the 4000-byte input does not fit mkbuf's small buffer: allocate it empty and write it */
	uint32_t binb = mkbuf(N * 4, NULL), bout = mkbuf(N * 4, NULL), bscale = mkbuf(4, &scale);
	{
		static uint8_t w[8 + N * 4];
		uint32_t zero = 0;

		memcpy(w, &binb, 4);
		memcpy(w + 4, &zero, 4);
		memcpy(w + 8, input, N * 4);
		if (call(3, w, sizeof w, NULL, NULL, NULL) != 0) {
			printf("mtlc: write buffer failed\n");
			return 6;
		}
	}
	uint32_t d[17] = { pso, 3, N, 1, 1, 64, 1, 1, binb, 0, 0, bout, 1, 0, bscale, 2, 0 };
	double t0 = now();

	if (call(6, d, sizeof d, NULL, NULL, NULL) != 0) {
		printf("mtlc: dispatch failed\n");
		return 7;
	}
	uint32_t rd[3] = { bout, 0, N * 4 }, n = 0;

	if (call(4, rd, sizeof rd, NULL, output, &n) != 0 || n != N * 4) {
		printf("mtlc: read back failed (%u bytes)\n", n);
		return 8;
	}
	double t1 = now();

	for (int i = 0; i < N; i++) {
		if (output[i] != 3.0f * (float)i) {
			printf("mtlc: output[%d] is %f, not %f\n", i, output[i], 3.0f * (float)i);
			return 9;
		}
	}
	printf("mtlc: compute on the Linux GPU from an iOS process: output[999] = %.0f as expected, dispatch + read back %.1f ms\n",
	    output[N - 1], (t1 - t0) * 1000);
	for (;;) {
		sleep(60);
	}
}
