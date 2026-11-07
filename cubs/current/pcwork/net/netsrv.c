/*
 * netsrv: Android's end of en0 (see redwood_cubs_net.c). Frames from iOS arrive in a ring in the RAM Linux has reserved (/dev/cubs_ram, offset 0x1000000):
 *   ARP for the gateway is answered here; DHCP is answered here (iOS gets 10.77.0.2/24, gateway 10.77.0.1, DNS 1.1.1.1 and 8.8.8.8 by NAT); other IPv4 goes to the tun device ios0, and what comes back from ios0 goes into the ring as Ethernet frames.
 * Android's own routing then takes it out through Wi-Fi or mobile data (netsrv.sh sets up forwarding, NAT and the policy rule).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define NET_OFF 0x1000000UL
#define SLOTS 128
#define SLOT 2048
#define TXO 0x1000
#define RXO 0x41000
#define MAGIC 0x3154454eu

static int ring_fd, tun_fd;
static uint8_t gw_mac[6] = { 0x02, 0x50, 0x58, 0x4c, 0x00, 0x01 };
static uint8_t ios_mac[6];
static int ios_mac_known;
static const uint8_t gw_ip[4] = { 10, 77, 0, 1 }, ios_ip[4] = { 10, 77, 0, 2 };
static uint32_t tx_cons, rx_prod;
static unsigned long polls, n_in, n_out, n_arp, n_dhcp, n_tun_in, n_drop;

static void
rd(uint32_t off, void *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pread(ring_fd, (char *)p + done, n - done, NET_OFF + off + done);

		if (r <= 0) {
			if (r < 0 && errno == EINTR) {
				continue;
			}
			memset((char *)p + done, 0, n - done);
			return;
		}
		done += (size_t)r;
	}
}

static void
wr(uint32_t off, const void *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pwrite(ring_fd, (const char *)p + done, n - done, NET_OFF + off + done);

		if (r <= 0) {
			if (r < 0 && errno == EINTR) {
				continue;
			}
			return;
		}
		done += (size_t)r;
	}
}

static uint32_t
word(uint32_t off)
{
	uint32_t v;

	rd(off, &v, 4);
	return v;
}

static void
setword(uint32_t off, uint32_t v)
{
	wr(off, &v, 4);
}

/* a frame towards iOS */
static void
to_ios(const uint8_t *frame, size_t len)
{
	uint32_t cons = word(0x100);

	if (len > SLOT - 4 || rx_prod - cons >= SLOTS) {
		n_drop++;
		return;
	}
	uint32_t off = RXO + (rx_prod % SLOTS) * SLOT, l = (uint32_t)len;

	wr(off + 4, frame, len);
	wr(off, &l, 4);
	rx_prod++;
	setword(0xC0, rx_prod);
	n_out++;
}

static uint16_t
csum(const void *data, size_t len, uint32_t sum)
{
	const uint8_t *p = data;

	for (; len > 1; len -= 2, p += 2) {
		sum += (uint32_t)(p[0] << 8 | p[1]);
	}
	if (len) {
		sum += (uint32_t)(p[0] << 8);
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static void
arp_reply(const uint8_t *f, size_t len)
{
	uint8_t r[60];

	if (len < 42 || f[20] != 0 || f[21] != 1 || memcmp(f + 38, gw_ip, 4) != 0) {      /* a request for the gateway */
		return;
	}
	memset(r, 0, sizeof r);
	memcpy(r, f + 6, 6);
	memcpy(r + 6, gw_mac, 6);
	r[12] = 8; r[13] = 6;
	memcpy(r + 14, f + 14, 6);                  /* hw/proto type, sizes */
	r[20] = 0; r[21] = 2;
	memcpy(r + 22, gw_mac, 6);
	memcpy(r + 28, gw_ip, 4);
	memcpy(r + 32, f + 22, 6);
	memcpy(r + 38, f + 28, 4);
	n_arp++;
	to_ios(r, 60);
}

static void
dhcp_reply(const uint8_t *f, size_t len)
{
	const uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
	uint8_t msg = 0;
	uint8_t r[400];

	if (len < 14 + 20 + 8 + 240 || (ip[0] & 0xf) != 5 || ip[9] != 17 || (udp[0] << 8 | udp[1]) != 68 || (udp[2] << 8 | udp[3]) != 67 || b[0] != 1) {
		return;
	}
	for (const uint8_t *o = b + 240; o + 2 <= f + len && *o != 255;) {
		if (*o == 0) { o++; continue; }
		if (*o == 53 && o[1] == 1) { msg = o[2]; }
		o += 2 + o[1];
	}
	if (msg != 1 && msg != 3) {
		return;
	}
	memset(r, 0, sizeof r);
	uint8_t *rip = r + 14, *rudp = rip + 20, *rb = rudp + 8;

	rb[0] = 2; rb[1] = 1; rb[2] = 6;
	memcpy(rb + 4, b + 4, 4);                   /* xid */
	memcpy(rb + 16, ios_ip, 4);                 /* yiaddr */
	memcpy(rb + 20, gw_ip, 4);                  /* siaddr */
	memcpy(rb + 28, b + 28, 6);                 /* chaddr */
	rb[236] = 99; rb[237] = 130; rb[238] = 83; rb[239] = 99;
	uint8_t *o = rb + 240;

	*o++ = 53; *o++ = 1; *o++ = msg == 1 ? 2 : 5;
	*o++ = 54; *o++ = 4; memcpy(o, gw_ip, 4); o += 4;
	*o++ = 51; *o++ = 4; *o++ = 0; *o++ = 1; *o++ = 0x51; *o++ = 0x80;    /* a day */
	*o++ = 1; *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
	*o++ = 3; *o++ = 4; memcpy(o, gw_ip, 4); o += 4;
	*o++ = 6; *o++ = 8; *o++ = 1; *o++ = 1; *o++ = 1; *o++ = 1; *o++ = 8; *o++ = 8; *o++ = 8; *o++ = 8;
	*o++ = 26; *o++ = 2; *o++ = 0x05; *o++ = 0xdc;                          /* MTU 1500 */
	*o++ = 255;
	size_t blen = (size_t)(o - rb);

	if (blen < 300) { blen = 300; }
	size_t ulen = 8 + blen, iplen = 20 + ulen;

	rip[0] = 0x45; rip[2] = (uint8_t)(iplen >> 8); rip[3] = (uint8_t)iplen; rip[8] = 64; rip[9] = 17;
	memcpy(rip + 12, gw_ip, 4);
	memcpy(rip + 16, ios_ip, 4);
	uint16_t c = csum(rip, 20, 0);

	rip[10] = (uint8_t)(c >> 8); rip[11] = (uint8_t)c;
	rudp[0] = 0; rudp[1] = 67; rudp[2] = 0; rudp[3] = 68; rudp[4] = (uint8_t)(ulen >> 8); rudp[5] = (uint8_t)ulen;      /* (UDP checksum 0: none) */
	memcpy(r, b + 28, 6);                       /* to the client's hardware address */
	memcpy(r + 6, gw_mac, 6);
	r[12] = 8; r[13] = 0;
	n_dhcp++;
	to_ios(r, 14 + iplen);
	printf("netsrv: DHCP %s answered\n", msg == 1 ? "discover" : "request");
}

static void
from_ios(const uint8_t *f, size_t len)
{
	uint16_t type;

	if (len < 14) {
		return;
	}
	memcpy(ios_mac, f + 6, 6);
	ios_mac_known = 1;
	type = (uint16_t)(f[12] << 8 | f[13]);
	n_in++;
	if (type == 0x0806) {
		arp_reply(f, len);
	} else if (type == 0x0800 && len >= 14 + 20) {
		if (f[14 + 9] == 17 && (f[14 + 20] << 8 | f[14 + 21]) == 68 && (f[14 + 22] << 8 | f[14 + 23]) == 67) {
			dhcp_reply(f, len);
		} else if (write(tun_fd, f + 14, len - 14) < 0) {
			n_drop++;
		}
	}
}

/* Android's clock for iOS (it has no RTC and would start in 1970, where no certificate is valid): the kernel (CUBS time) reads "TIME<seconds>" here */
static void
put_time(void)
{
	char t[32];
	int n = snprintf(t, sizeof t, "TIME%ld\n", (long)time(NULL));

	(void)pwrite(ring_fd, t, (size_t)n + 1, 0xFF1000);
}

int
main(void)
{
	struct ifreq ifr;
	time_t last = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	ring_fd = open("/dev/cubs_ram", O_RDWR);
	if (ring_fd < 0) {
		perror("netsrv: open /dev/cubs_ram");
		return 1;
	}
	tun_fd = open("/dev/tun", O_RDWR | O_NONBLOCK);
	if (tun_fd < 0) {
		perror("netsrv: open /dev/tun");
		return 1;
	}
	memset(&ifr, 0, sizeof ifr);
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strcpy(ifr.ifr_name, "ios0");
	if (ioctl(tun_fd, TUNSETIFF, &ifr) < 0) {
		perror("netsrv: TUNSETIFF");
		return 1;
	}
	setword(4, 0);
	tx_cons = word(0x40);                       /* start from where iOS is */
	rx_prod = word(0xC0);
	setword(0x80, tx_cons);
	setword(0, MAGIC);
	printf("netsrv: ready (tx %u rx %u)\n", tx_cons, rx_prod);
	put_time();
	for (;;) {
		int busy = 0;
		uint32_t prod = word(0x40);

		while (tx_cons != prod) {
			uint32_t off = TXO + (tx_cons % SLOTS) * SLOT, len = word(off);
			uint8_t buf[SLOT];

			if (len >= 14 && len <= SLOT - 4) {
				rd(off + 4, buf, len);
				from_ios(buf, len);
			}
			tx_cons++;
			setword(0x80, tx_cons);
			busy = 1;
		}
		for (int i = 0; i < 32; i++) {
			uint8_t pkt[2048], fr[2048 + 14];
			ssize_t n = read(tun_fd, pkt, sizeof pkt);

			if (n <= 0) {
				break;
			}
			if ((pkt[0] >> 4) != 4 || !ios_mac_known || (size_t)n + 14 > sizeof fr) {
				continue;
			}
			memcpy(fr, ios_mac, 6);
			memcpy(fr + 6, gw_mac, 6);
			fr[12] = 8; fr[13] = 0;
			memcpy(fr + 14, pkt, (size_t)n);
			n_tun_in++;
			to_ios(fr, (size_t)n + 14);
			busy = 1;
		}
		if (time(NULL) - last >= 30) {
			last = time(NULL);
			put_time();
			printf("netsrv: from iOS %lu, to iOS %lu (from tun %lu), arp %lu, dhcp %lu, dropped %lu; ring magic %#x xnu_up %u tx_prod %u rx_cons %u\n", n_in, n_out, n_tun_in, n_arp, n_dhcp, n_drop,
			    word(0), word(4), word(0x40), word(0x100));
		}
		/* BYE1 in the return page (XNU: power + volume down held): sync and reboot into Android */
		if ((++polls % 500) == 0) {
			uint32_t v = 0;

			if (pread(ring_fd, &v, 4, 0xFF0000) == 4 && v == 0x31455942) {
				printf("netsrv: BYE1 seen, rebooting into Android\n");
				sync();
				system("/system/bin/reboot");
				sleep(30);
			}
		}
		if (!busy) {
			usleep(1000);
		}
	}
}
