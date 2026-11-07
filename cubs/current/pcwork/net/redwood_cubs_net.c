/*
 * The link of en0 on the Pixel: Ethernet frames carried through a shared-memory ring to a server on Android (netsrv), which answers ARP and DHCP,
 * and forwards the IPv4 traffic through a tun device and Android's own network (Wi-Fi or mobile data).
 *
 * The ring is in the RAM Linux has reserved for iOS (PA 0xab2000000, the span the GPU servers use), at +0x1000000:
 *   +0x0000  header: u32 magic "NET1" (written by netsrv when it is up); u32 xnu_up (written here)
 *   +0x0040  tx_prod (here)   +0x0080 tx_cons (netsrv)   +0x00C0 rx_prod (netsrv)   +0x0100 rx_cons (here)    (a cache line each)
 *   +0x1000  tx slots (iOS -> Android): NET_SLOTS of NET_SLOT bytes: u32 length, the frame
 *   +0x41000 rx slots (Android -> iOS)
 * Boot-arg cubs_net=1 turns it on. Called from redwood_net_init().
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>
#include <vm/pmap.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include "redwood_scan_decode.h"
#include "redwood_join_security.h"

extern void *io_map(vm_map_offset_t phys_addr, vm_size_t size, unsigned int flags, vm_prot_t prot, bool unmappable);
extern void FlushPoC_DcacheRegion(vm_offset_t va, unsigned length);

#define NET_PA          (0xab2000000ULL + 0x1000000ULL)
#define NET_SLOTS       128
#define NET_SLOT        2048
#define NET_TX          0x1000
#define NET_RX          0x41000
#define NET_SIZE        0x81000
#define NET_MAGIC       0x3154454eu     /* "NET1" */

/* Experimental read-only view of Android's actual Wi-Fi connection. */
#define WIFI_PA         (0xab2000000ULL + 0x2000000ULL)
#define WIFI_MAGIC      0x31464957u
struct cubs_wifi_wire {
	uint32_t magic, sequence, powered, connected, frequency;
	int32_t rssi;
	uint32_t ssid_length;
	int32_t security;
	uint32_t elements_length;
	uint8_t bssid[6], ssid[32], elements[1024];
};
_Static_assert(sizeof(struct cubs_wifi_wire) == 1100, "Wi-Fi status format");
static volatile struct cubs_wifi_wire *cw_wire;
#define CW_JOIN_PA (0xab2000000ULL + 0x2200000ULL)
static volatile uint8_t *cw_join_ring;
static uint32_t cw_join_enabled;
#define CW_SCAN_PA (0xab2000000ULL + 0x2100000ULL)
#define CW_SCAN_MAP_SIZE 0x12000u
#define CW_SCAN_MAX (16u + 64u * (28u + 32u + 1024u))
static volatile uint8_t *cw_scan_ring;
static uint8_t cw_scan_data[CW_SCAN_MAX];
static uint32_t cw_scan_size, cw_scan_seen;
static struct redwood_sta_scan cw_scan_filter;

static bool
cw_snapshot(struct cubs_wifi_wire *out)
{
	uint32_t a, b;
	if (!cw_wire) return false;
	FlushPoC_DcacheRegion((vm_offset_t)cw_wire, sizeof(*out));
	a = cw_wire->sequence;
	if (!a || (a & 1)) return false;
	__asm__ volatile ("dmb ishld" ::: "memory");
	memcpy(out, (const void *)(uintptr_t)cw_wire, sizeof(*out));
	__asm__ volatile ("dmb ishld" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)cw_wire, sizeof(*out));
	b = cw_wire->sequence;
	return a == b && !(b & 1) && out->magic == WIFI_MAGIC && out->ssid_length <= sizeof(out->ssid) &&
	    out->elements_length <= sizeof(out->elements);
}

static int cw_power(bool on) { struct cubs_wifi_wire w; return on && cw_snapshot(&w) && w.powered ? 0 : EOPNOTSUPP; }
static int
cw_scan_take(const struct redwood_sta_scan *s, uint32_t baseline)
{
	uint32_t seq, end, len, fresh = 0;
	cw_scan_size = 0;
	FlushPoC_DcacheRegion((vm_offset_t)cw_scan_ring, CW_SCAN_MAP_SIZE);
	seq = *(volatile uint32_t *)(cw_scan_ring + 4);
	len = *(volatile uint32_t *)(cw_scan_ring + 8);
	if (*(volatile uint32_t *)cw_scan_ring != 0x31424353u || !seq || (seq & 1) || seq == baseline || seq == cw_scan_seen || len < 16 || len > CW_SCAN_MAX)
		return EAGAIN;
	__asm__ volatile ("dmb ishld" ::: "memory");
	memcpy(cw_scan_data, (const void *)(uintptr_t)(cw_scan_ring + 0x100), len);
	__asm__ volatile ("dmb ishld" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)cw_scan_ring, 16);
	end = *(volatile uint32_t *)(cw_scan_ring + 4);
	if (end != seq || (end & 1) || cubs_scan_parse(cw_scan_data, len, NULL, NULL, &fresh, NULL) || !fresh)
		return EAGAIN;
	cw_scan_seen = seq;
	cw_scan_filter = *s;
	cw_scan_size = len;
	return 0;
}
static int
cw_scan(const struct redwood_sta_scan *s)
{
	uint32_t baseline, request;
	uint64_t deadline;
	cw_scan_size = 0;
	if (!cw_scan_ring || s->passive || s->dwell_ms || s->ssid_length || s->frequency_count > 64)
		return EOPNOTSUPP;
	FlushPoC_DcacheRegion((vm_offset_t)cw_scan_ring, 16);
	if (*(volatile uint32_t *)cw_scan_ring != 0x31424353u) return ENXIO;
	baseline = *(volatile uint32_t *)(cw_scan_ring + 4);
	request = *(volatile uint32_t *)(cw_scan_ring + 0x80) + 1;
	*(volatile uint32_t *)(cw_scan_ring + 0x80) = request;
	__asm__ volatile ("dmb ish" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)(cw_scan_ring + 0x80), 4);
	for (unsigned i=0; i<26; i++) {
		if (!cw_scan_take(s, baseline)) return 0;
		clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
		assert_wait_deadline((event_t)&cw_scan_ring, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
	return EAGAIN;
}
struct cw_heard_ctx { void (*each)(const struct redwood_sta_bss *, void *); void *context; };
static void
cw_heard_record(const struct cubs_scan_record *r, void *arg)
{
	struct cw_heard_ctx *ctx = arg;
	struct redwood_sta_bss b;
	if (cw_scan_filter.ssid_length && (r->ssid_length != cw_scan_filter.ssid_length || memcmp(r->ssid, cw_scan_filter.ssid, r->ssid_length))) return;
	if (cw_scan_filter.frequency_count) {
		bool found = false;
		for (unsigned i = 0; i < cw_scan_filter.frequency_count; i++) if (cw_scan_filter.frequencies[i] == r->frequency) { found = true; break; }
		if (!found) return;
	}
	bzero(&b, sizeof(b));
	memcpy(b.bssid, r->bssid, 6); memcpy(b.ssid, r->ssid, r->ssid_length);
	b.ssid_length = (uint8_t)r->ssid_length; b.frequency = (uint16_t)r->frequency;
	b.rssi = (int16_t)r->rssi; b.age_ms = r->age_ms; b.capability = r->capability;
	b.elements_length = r->elements_length; memcpy(b.elements, r->elements, r->elements_length);
	ctx->each(&b, ctx->context);
}
static void
cw_heard(void (*each)(const struct redwood_sta_bss *, void *), void *context)
{
	struct cw_heard_ctx ctx = { each, context };
	if (each && cw_scan_size) (void)cubs_scan_parse(cw_scan_data, cw_scan_size, cw_heard_record, &ctx, NULL, NULL);
}
static bool
cw_bssid_specified(const uint8_t *bssid)
{
	for (unsigned i=0; i<6; i++) if (bssid[i]) return true;
	return false;
}
static bool
cw_matches(const struct cubs_wifi_wire *w, const struct redwood_sta_join *j)
{
	if (!w->connected || w->ssid_length != j->ssid_length || memcmp(w->ssid,j->ssid,j->ssid_length)) return false;
	if (cw_bssid_specified(j->bssid) && memcmp(w->bssid,j->bssid,6)) return false;
	return j->security == REDWOOD_STA_OPEN ? w->security == 0 : w->security == 2;
}
static int
cw_join(const struct redwood_sta_join *j)
{
	/* Experimental only: do not enable until transport and association tests pass. */
	struct redwood_sta_bss b;
	struct cubs_wifi_wire before, after;
	unsigned char frame[109] = { 'J','O','N','1' };
	uint32_t request, response, status, previous_status = 0;
	uint64_t deadline;
	unsigned len, keylen, bssidlen;
	bool found = false;
	int result = EOPNOTSUPP;
	if (!cw_join_enabled || !cw_join_ring || !j->ssid_length || j->ssid_length > 32 ||
	    (j->security != REDWOOD_STA_OPEN && j->security != REDWOOD_STA_WPA2_PSK)) goto out;
	if (!cubs_join_auth_matches(j->security == REDWOOD_STA_OPEN ? CUBS_JOIN_OPEN : CUBS_JOIN_WPA2_PSK,
	    j->apple_auth_lower,j->apple_auth_upper)) goto out;
	/* The scan data must identify the requested security unambiguously. */
	for (unsigned i=0; redwood_sta_heard(i,&b); i++) {
		if (b.ssid_length != j->ssid_length || memcmp(b.ssid,j->ssid,j->ssid_length) ||
		    (cw_bssid_specified(j->bssid) && memcmp(b.bssid,j->bssid,6))) continue;
		enum cubs_join_security mode = cubs_join_security(b.capability,b.elements,b.elements_length);
		if ((j->security == REDWOOD_STA_OPEN && mode != CUBS_JOIN_OPEN) ||
		    (j->security == REDWOOD_STA_WPA2_PSK && mode != CUBS_JOIN_WPA2_PSK)) goto out;
		found = true;
	}
	if (!found) { result = ENOENT; goto out; }
	if (cw_snapshot(&before)) {
		previous_status = before.sequence;
		if (cw_matches(&before,j)) { result = EALREADY; goto out; }
	}
	bssidlen = cw_bssid_specified(j->bssid) ? 6 : 0;
	keylen = j->security == REDWOOD_STA_OPEN ? 0 : (j->passphrase_length ? j->passphrase_length : 32);
	if ((j->passphrase_length && (j->passphrase_length < 8 || j->passphrase_length > 63)) ||
	    (j->security == REDWOOD_STA_OPEN && j->passphrase_length)) { result = EINVAL; goto out; }
	frame[4] = j->ssid_length; frame[5] = j->security == REDWOOD_STA_OPEN ? 0 : (j->passphrase_length ? 1 : 2);
	frame[6] = (uint8_t)bssidlen; frame[7] = (uint8_t)keylen;
	len = 8; memcpy(frame+len,j->ssid,j->ssid_length); len += j->ssid_length;
	if (bssidlen) { memcpy(frame+len,j->bssid,bssidlen); len += bssidlen; }
	if (keylen) { memcpy(frame+len,j->passphrase_length ? j->passphrase : j->pmk,keylen); len += keylen; }
	FlushPoC_DcacheRegion((vm_offset_t)cw_join_ring,0x400);
	if (*(volatile uint32_t *)cw_join_ring != 0x3252424au) { result = ENXIO; goto out; }
	request = *(volatile uint32_t *)(cw_join_ring+0x100);
	response = *(volatile uint32_t *)(cw_join_ring+0x80);
	if ((request&1) || request != response) { result = EBUSY; goto out; }
	request += 2; if (!request) request = 2;
	*(volatile uint32_t *)(cw_join_ring+0x180) = 0;
	*(volatile uint32_t *)(cw_join_ring+0x100) = request-1;
	*(volatile uint32_t *)(cw_join_ring+0x104) = len;
	memcpy((void *)(uintptr_t)(cw_join_ring+0x200),frame,len);
	__asm__ volatile ("dmb ish" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x100),0x200);
	*(volatile uint32_t *)(cw_join_ring+0x100) = request;
	__asm__ volatile ("dmb ish" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x100),8);
	memset_s(frame,sizeof(frame),0,sizeof(frame));
	/* Claim alone does not authorize a join. Only commit after observing it. */
	result = ETIMEDOUT;
	for (unsigned i=0;i<40;i++) {
		FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0xc0),4);
		if (*(volatile uint32_t *)(cw_join_ring+0xc0)==request) { result=0; break; }
		FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x80),8);
		response=*(volatile uint32_t *)(cw_join_ring+0x80);
		status=*(volatile uint32_t *)(cw_join_ring+0x84);
		if (response==request) { result=EIO; break; }
		clock_interval_to_deadline(500,NSEC_PER_MSEC,&deadline);
		assert_wait_deadline((event_t)&cw_join_ring,THREAD_UNINT,deadline); thread_block(THREAD_CONTINUE_NULL);
	}
	*(volatile uint32_t *)(cw_join_ring+0x180) = result ? request|1u : request;
	__asm__ volatile ("dmb ish" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x180),4);
	if (result) goto clear;
	result = ETIMEDOUT;
	for (unsigned i=0;i<40;i++) {
		FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x80),8);
		response=*(volatile uint32_t *)(cw_join_ring+0x80);
		status=*(volatile uint32_t *)(cw_join_ring+0x84);
		if (response==request) { result=status==1 ? 0 : (status==2 ? EIO : ETIMEDOUT); break; }
		clock_interval_to_deadline(500,NSEC_PER_MSEC,&deadline);
		assert_wait_deadline((event_t)&cw_join_ring,THREAD_UNINT,deadline); thread_block(THREAD_CONTINUE_NULL);
	}
	/* An absent or delayed acknowledgement is ambiguous; never treat it as a join. */
	if (result) goto clear;
	result=ETIMEDOUT;
	for (unsigned i=0;i<60;i++) {
		if (cw_snapshot(&after) && after.sequence != previous_status && cw_matches(&after,j)) { result=0; break; }
		clock_interval_to_deadline(500,NSEC_PER_MSEC,&deadline);
		assert_wait_deadline((event_t)&cw_join_ring,THREAD_UNINT,deadline); thread_block(THREAD_CONTINUE_NULL);
	}
clear:
	memset((void *)(uintptr_t)(cw_join_ring+0x200),0,sizeof(frame));
	__asm__ volatile ("dmb ish" ::: "memory");
	FlushPoC_DcacheRegion((vm_offset_t)(cw_join_ring+0x200),sizeof(frame));
out:
	memset_s(frame,sizeof(frame),0,sizeof(frame));
	memset_s(&b,sizeof(b),0,sizeof(b));
	return result;
}
static int cw_leave(void) { return EOPNOTSUPP; }
static void
cw_state(struct redwood_sta_state *state)
{
	struct cubs_wifi_wire w;
	bzero(state, sizeof(*state));
	if (!cw_snapshot(&w) || !w.powered) { state->phase = REDWOOD_STA_OFF; return; }
	state->phase = w.connected ? REDWOOD_STA_JOINED : REDWOOD_STA_IDLE;
	if (!w.connected) return;
	memcpy(state->bss.bssid, w.bssid, sizeof(w.bssid));
	memcpy(state->bss.ssid, w.ssid, w.ssid_length);
	state->bss.ssid_length = (uint8_t)w.ssid_length;
	state->bss.frequency = (uint16_t)w.frequency;
	state->bss.rssi = (int16_t)w.rssi;
	state->bss.capability = w.security > 0 ? 0x10 : 0;
	state->bss.elements_length = (uint16_t)w.elements_length;
	memcpy(state->bss.elements, w.elements, w.elements_length);
}
static unsigned
cw_channels(struct redwood_sta_channel *out, unsigned max)
{
	struct cubs_wifi_wire w;
	if (!max || !cw_snapshot(&w) || !w.connected) return 0;
	out[0].frequency = (uint16_t)w.frequency; out[0].flags = 0;
	return 1;
}
static const struct redwood_sta_ops cw_ops = {
	.power = cw_power, .scan = cw_scan, .heard = cw_heard, .join = cw_join, .leave = cw_leave,
	.state = cw_state, .channels = cw_channels, .address = { 0x02, 0x50, 0x58, 0x4c, 0x00, 0x02 }
};
static uint32_t cw_enabled;

static volatile uint8_t *cn_ring;
static uint32_t cn_tx_prod, cn_rx_cons;
static uint32_t cn_sent, cn_got, cn_full;

static inline volatile uint32_t *
cn_w(uint32_t off)
{
	return (volatile uint32_t *)(cn_ring + off);
}

static void
cn_dsb(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
cn_transmit(mbuf_t packet, size_t length)
{
	uint32_t cons, off;
	volatile uint8_t *slot;

	if (cn_ring == NULL || length > NET_SLOT - 4) {
		return false;
	}
	FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	cons = *cn_w(0x80);
	if (cn_tx_prod - cons >= NET_SLOTS) {
		cn_full++;
		return false;
	}
	off = NET_TX + (cn_tx_prod % NET_SLOTS) * NET_SLOT;
	slot = cn_ring + off;
	if (mbuf_copydata(packet, 0, length, (void *)(slot + 4)) != 0) {
		return false;
	}
	*(volatile uint32_t *)slot = (uint32_t)length;
	cn_dsb();
	FlushPoC_DcacheRegion((vm_offset_t)slot, (unsigned)(length + 4));
	cn_dsb();
	cn_tx_prod++;
	*cn_w(0x40) = cn_tx_prod;
	cn_dsb();
	FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	cn_sent++;
	return true;
}

static unsigned
cn_poll(void)
{
	unsigned n = 0;
	uint32_t prod;

	if (cn_ring == NULL) {
		return 0;
	}
	FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	prod = *cn_w(0xC0);
	while (cn_rx_cons != prod && n < 64) {
		volatile uint8_t *slot = cn_ring + NET_RX + (cn_rx_cons % NET_SLOTS) * NET_SLOT;
		uint32_t length;

		FlushPoC_DcacheRegion((vm_offset_t)slot, NET_SLOT);
		length = *(volatile uint32_t *)slot;
		if (length >= 14 && length <= NET_SLOT - 4) {
			redwood_net_input((const uint8_t *)(slot + 4), length);
			cn_got++;
		}
		cn_rx_cons++;
		n++;
	}
	if (n) {
		*cn_w(0x100) = cn_rx_cons;
		cn_dsb();
		FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	}
	return n;
}

static struct redwood_net_link cn_link = {
	.name = "the ring to Android (netsrv)",
	.address = { 0x02, 0x50, 0x58, 0x4c, 0x00, 0x02 },
	.wireless = false,
	.up = true,
	.transmit = cn_transmit,
	.poll = cn_poll,
};

static thread_call_t cn_call;

static void
cn_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (cn_ring == NULL) {
		cn_ring = (volatile uint8_t *)io_map(NET_PA, NET_SIZE, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
		if (cn_ring == NULL) {
			printf("net: the ring to Android cannot be mapped\n");
			return;
		}
	}
	FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	if (*cn_w(0) != NET_MAGIC) {
		/* netsrv is not up (yet): try again in a second */
		static unsigned said;

		if ((said++ % 10) == 0) {
			printf("net: the ring to Android: magic %#x (want %#x), words %#x %#x %#x\n", *cn_w(0), NET_MAGIC, *cn_w(4), *cn_w(0x40), *cn_w(0xC0));
		}
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(cn_call, deadline);
		return;
	}
	cn_tx_prod = *cn_w(0x40);
	cn_rx_cons = *cn_w(0xC0);
	*cn_w(0x100) = cn_rx_cons;
	*cn_w(4) = 1;
	cn_dsb();
	FlushPoC_DcacheRegion((vm_offset_t)cn_ring, 256);
	if (cw_enabled) {
		struct cubs_wifi_wire w;
		static unsigned waits;
		if (!cw_wire) cw_wire = (volatile struct cubs_wifi_wire *)io_map(WIFI_PA, 0x4000, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
		if (!cw_scan_ring) {
			cw_scan_ring = (volatile uint8_t *)io_map(CW_SCAN_PA, CW_SCAN_MAP_SIZE, VM_WIMG_DEFAULT, VM_PROT_DEFAULT, false);
			if (cw_scan_ring) {
				FlushPoC_DcacheRegion((vm_offset_t)cw_scan_ring, 16);
				cw_scan_seen = *(volatile uint32_t *)(cw_scan_ring + 4);
			}
		}
		if (cw_snapshot(&w) && w.connected) {
			cn_link.wireless = true;
			redwood_sta_attach(&cw_ops);
			printf("cubs wifi: experimental Android Wi-Fi status attached\n");
		} else if (waits++ < 40) {
			clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
			thread_call_enter_delayed(cn_call, deadline);
			return;
		} else {
			printf("cubs wifi: status unavailable; retaining Ethernet\n");
		}
	}
	if (!redwood_net_attach(&cn_link)) {
		printf("net: no network interface (the ring)\n");
	}
}

void
redwood_cubs_net_init(void)
{
	uint32_t enabled = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("cubs_net", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	(void)PE_parse_boot_argn("cubs_wifi", &cw_enabled, sizeof(cw_enabled));
	(void)PE_parse_boot_argn("cubs_join", &cw_join_enabled, sizeof(cw_join_enabled));
	if (cw_join_enabled) cw_join_ring = (volatile uint8_t *)io_map(CW_JOIN_PA,0x4000,VM_WIMG_DEFAULT,VM_PROT_DEFAULT,false);
	cn_call = thread_call_allocate(cn_start, NULL);
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(cn_call, deadline);
}
