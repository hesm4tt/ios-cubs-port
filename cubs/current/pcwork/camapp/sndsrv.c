/*
 * sndsrv: Android's end of the sound of iOS. iOS's PCM (48 kHz, 16-bit stereo) is in a ring in the reserved RAM (/dev/cubs_ram, offset 0x1A00000, see
 * bsd/dev/redwood_cubs_sound.c); it is sent to the app com.cubs.cam's SndService, which plays it, over 127.0.0.1:7778. While nobody listens the ring is emptied.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OFF 0x1A00000UL
#define RING_FRAMES 16384
#define MAGIC 0x31445541u

static int rfd;

static uint32_t
word(uint32_t o)
{
	uint32_t v = 0;

	(void)pread(rfd, &v, 4, OFF + o);
	return v;
}

static void
setword(uint32_t o, uint32_t v)
{
	(void)pwrite(rfd, &v, 4, OFF + o);
}

int
main(void)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, cl = -1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7778), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	static uint8_t buf[RING_FRAMES * 4];
	unsigned long sent = 0, last = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	rfd = open("/dev/cubs_ram", O_RDWR);
	if (rfd < 0) {
		perror("sndsrv: /dev/cubs_ram");
		return 1;
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
		perror("sndsrv: bind");
		return 1;
	}
	fcntl(ls, F_SETFL, O_NONBLOCK);
	setword(0x80, word(0x40));
	setword(0, MAGIC);
	printf("sndsrv: ready\n");
	for (;;) {
		uint32_t wr = word(0x40), rd = word(0x80), avail = wr - rd;

		if (cl < 0) {
			cl = accept(ls, NULL, NULL);
			if (cl >= 0) {
				printf("sndsrv: player connected\n");
			}
		}
		if (cl < 0 || avail == 0) {
			if (cl < 0 && avail) {
				setword(0x80, wr);              /* nobody plays: empty the ring */
			}
			usleep(5000);
			continue;
		}
		if (avail > RING_FRAMES) {
			avail = RING_FRAMES;
			rd = wr - avail;
		}
		uint32_t n = avail > 2048 ? 2048 : avail, at = rd % RING_FRAMES, first = RING_FRAMES - at < n ? RING_FRAMES - at : n;

		(void)pread(rfd, buf, first * 4, OFF + 0x1000 + (off_t)at * 4);
		if (first < n) {
			(void)pread(rfd, buf + first * 4, (n - first) * 4, OFF + 0x1000);
		}
		if (send(cl, buf, n * 4, MSG_NOSIGNAL) < 0) {
			printf("sndsrv: player gone\n");
			close(cl);
			cl = -1;
		}
		sent += n;
		setword(0x80, rd + n);
		if (sent - last >= 48000) {
			last = sent;
			printf("sndsrv: %lu frames sent\n", sent);
		}
	}
}
