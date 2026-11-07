/*
 * libiospy.dylib: injected into lsd (DYLD_INSERT_LIBRARIES). libMobileGestalt waits in MGIOKitSupport for IORegistry services to
 * match (a run loop with a notification port) and during iOS first boot on this port some never appear. This prints to
 * /dev/console what is asked for: IOServiceMatching / IOServiceNameMatching names, IOServiceAddMatchingNotification (the
 * matching dictionary), IOServiceGetMatchingService(s) and IORegistryEntryFromPath, and the registry paths that are wanted.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_service_t;
typedef io_object_t io_registry_entry_t;
typedef struct IONotificationPort *IONotificationPortRef;
typedef void (*IOServiceMatchingCallback)(void *refcon, io_iterator_t iterator);

extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern CFMutableDictionaryRef IOServiceNameMatching(const char *name);
extern kern_return_t IOServiceAddMatchingNotification(IONotificationPortRef, const char *, CFDictionaryRef, IOServiceMatchingCallback,
    void *, io_iterator_t *);
extern io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef);
extern kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, io_iterator_t *);
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t, const char *);

static int con = -2;
static volatile int count;

static void
say(const char *what, const char *a, CFTypeRef dict)
{
	char b[500];
	char d[300] = "";

	if (con == -2) {
		con = open("/dev/console", O_WRONLY);
	}
	if (con < 0 || count >= 400) {
		return;
	}
	count++;
	if (dict) {
		CFStringRef s = CFCopyDescription(dict);

		if (s) {
			CFStringGetCString(s, d, sizeof d, kCFStringEncodingUTF8);
			CFRelease(s);
			for (char *p = d; *p; p++) {
				if (*p == '\n' || *p == '\t') {
					*p = ' ';
				}
			}
		}
	}
	int n = snprintf(b, sizeof b, "iospy: %s %s %s\n", what, a ? a : "", d);

	(void)write(con, b, (size_t)n);
}

static CFMutableDictionaryRef
my_matching(const char *name)
{
	say("IOServiceMatching", name, NULL);
	return IOServiceMatching(name);
}

static CFMutableDictionaryRef
my_name_matching(const char *name)
{
	say("IOServiceNameMatching", name, NULL);
	return IOServiceNameMatching(name);
}

static kern_return_t
my_add_notification(IONotificationPortRef p, const char *type, CFDictionaryRef m, IOServiceMatchingCallback cb, void *ref,
    io_iterator_t *it)
{
	say("IOServiceAddMatchingNotification", type, m);
	return IOServiceAddMatchingNotification(p, type, m, cb, ref, it);
}

static io_service_t
my_get_service(mach_port_t port, CFDictionaryRef m)
{
	io_service_t r = IOServiceGetMatchingService(port, m);

	say(r ? "IOServiceGetMatchingService found" : "IOServiceGetMatchingService MISSING", NULL, m);
	return r;
}

static kern_return_t
my_get_services(mach_port_t port, CFDictionaryRef m, io_iterator_t *it)
{
	say("IOServiceGetMatchingServices", NULL, m);
	return IOServiceGetMatchingServices(port, m, it);
}

static io_registry_entry_t
my_from_path(mach_port_t port, const char *path)
{
	io_registry_entry_t r = IORegistryEntryFromPath(port, path);

	say(r ? "IORegistryEntryFromPath found" : "IORegistryEntryFromPath MISSING", path, NULL);
	return r;
}

#define INTERPOSE(new, old) { (const void *)(uintptr_t)(new), (const void *)(uintptr_t)(old) }
__attribute__((used)) static const struct {
	const void *replacement, *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	INTERPOSE(my_matching, IOServiceMatching),
	INTERPOSE(my_name_matching, IOServiceNameMatching),
	INTERPOSE(my_add_notification, IOServiceAddMatchingNotification),
	INTERPOSE(my_get_service, IOServiceGetMatchingService),
	INTERPOSE(my_get_services, IOServiceGetMatchingServices),
	INTERPOSE(my_from_path, IORegistryEntryFromPath),
};
