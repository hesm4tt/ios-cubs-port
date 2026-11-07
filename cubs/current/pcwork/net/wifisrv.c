/* Forward authenticated local app Wi-Fi snapshots into the iOS shared ring. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define OFF 0x2000000
#define MAGIC 0x31464957u
struct wifi_wire {
	uint32_t magic, sequence, powered, connected, frequency;
	int32_t rssi;
	uint32_t ssid_length;
	int32_t security;
	uint32_t elements_length;
	uint8_t bssid[6], ssid[32], elements[1024];
};
_Static_assert(sizeof(struct wifi_wire) == 1100, "wire format");
static int ring;
static uint32_t sequence;

static int publish(struct wifi_wire *w)
{
	uint32_t odd = sequence + 1;
	w->magic = MAGIC;
	if (pwrite(ring, &odd, 4, OFF + 4) != 4 || pwrite(ring, &w->magic, 4, OFF) != 4 ||
	    pwrite(ring, &w->powered, sizeof(*w) - 8, OFF + 8) != (ssize_t)sizeof(*w) - 8) return -1;
	sequence += 2;
	return pwrite(ring, &sequence, 4, OFF + 4) == 4 ? 0 : -1;
}

static int read_packet(int fd, void *data, size_t n)
{
	size_t have = 0;
	while (have < n) {
		ssize_t r = read(fd, (char *)data + have, n - have);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		have += (size_t)r;
	}
	return 0;
}

int main(int argc, char **argv)
{
	char *end = NULL;
	if (argc != 2) return 2;
	unsigned long expected_uid = strtoul(argv[1], &end, 10);
	if (!end || *end || expected_uid < 10000 || expected_uid > 999999) return 2;
	const char *path = getenv("CUBS_WIFI_RING");
	ring = open(path ? path : "/dev/cubs_ram", O_RDWR);
	if (ring < 0) { perror("wifisrv: ring"); return 1; }
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char *name = "cubs_wifi_status";
	memcpy(address.sun_path + 1, name, strlen(name));
	if (ls < 0 || bind(ls, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)) || listen(ls, 1)) {
		perror("wifisrv: listen"); return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct wifi_wire empty = {0};
	if (publish(&empty)) return 1;
	printf("wifisrv: waiting for app\n");
	for (;;) {
		int cl = accept(ls, NULL, NULL);
		if (cl < 0) { if (errno == EINTR) continue; return 1; }
		struct ucred cred; socklen_t len = sizeof(cred);
		if (getsockopt(cl, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != expected_uid) { close(cl); continue; }
		struct timeval timeout = { .tv_sec = 20 };
		setsockopt(cl, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		printf("wifisrv: app connected\n");
		struct wifi_wire w; unsigned count = 0;
		uint32_t last_powered = 2, last_connected = 2, last_has_elements = 2;
		while (read_packet(cl, &w, sizeof(w)) == 0) {
			if (w.magic != MAGIC || w.powered > 1 || w.connected > 1 || w.ssid_length > sizeof(w.ssid) ||
			    w.elements_length > sizeof(w.elements) || (w.connected && (!w.ssid_length || w.frequency < 2400 || w.frequency > 7125))) break;
			if (publish(&w)) return 1;
			if (!count++ || w.powered != last_powered || w.connected != last_connected || !!w.elements_length != last_has_elements) {
				printf("wifisrv: status powered=%u connected=%u security=%d security-data=%u\n", w.powered, w.connected, w.security, w.elements_length);
				last_powered = w.powered; last_connected = w.connected; last_has_elements = !!w.elements_length;
			}
		}
		close(cl);
		memset(&empty, 0, sizeof(empty));
		if (publish(&empty)) return 1;
		printf("wifisrv: app disconnected\n");
	}
}
