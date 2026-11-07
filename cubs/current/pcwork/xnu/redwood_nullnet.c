/*
 * boot-arg net_null=1: an en0 with no network behind it (see sys/redwood_net.h). iOS first boot on a phone that has no link driver
 * (no QEMU virtio network device, no radio) has MobileGestaltHelper wait ~16 minutes for the Wi-Fi address (WifiAddress and
 * UniqueDeviceID never answer), lsd blocks on it and SpringBoard waits for lsd. With this link en0 exists and has an address;
 * every frame sent is dropped, the cable is "unplugged".
 *
 * To build in: add "bsd/dev/redwood_nullnet.c standard" to bsd/conf/files and call redwood_nullnet_init() after
 * redwood_net_init() in redwood_input_init() (bsd/dev/redwood_input.c).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/redwood_net.h>
#include <sys/systm.h>

static bool
nullnet_transmit(__unused mbuf_t packet, __unused size_t length)
{
	return false;
}

static struct redwood_net_link null_link = {
	.name = "a link to nowhere",
	.address = { 0x02, 'c', 'u', 'b', 's', 0x01 },
	.wireless = false,
	.up = false,
	.transmit = nullnet_transmit,
	.poll = NULL,
};

void redwood_nullnet_init(void);

void
redwood_nullnet_init(void)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("net_null", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (!redwood_net_attach(&null_link)) {
		printf("net: the link to nowhere is not attached\n");
	}
}
