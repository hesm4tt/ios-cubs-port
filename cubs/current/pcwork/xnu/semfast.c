/*
 * libsemfast.dylib: injected into lsd (DYLD_INSERT_LIBRARIES, __interpose). During iOS first boot lsd blocks in libMobileGestalt on
 * a dispatch semaphore that is never signalled (a MobileGestaltHelper answer that does not come) and SpringBoard waits for lsd.
 * Here every dispatch_semaphore_wait with DISPATCH_TIME_FOREVER becomes a wait of SEM_SECONDS, and the first waits are logged with
 * their caller, to see who waits for what.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SEM_SECONDS 1

static int con = -2;
static volatile int logged;

static void
note(void *caller)
{
	Dl_info di;
	char b[240];
	int n;

	if (con == -2) {
		con = open("/dev/console", O_WRONLY);
	}
	if (con < 0 || logged >= 60) {
		return;
	}
	logged++;
	if (dladdr(caller, &di) && di.dli_fname) {
		const char *base = strrchr(di.dli_fname, '/');

		n = snprintf(b, sizeof b, "semfast: forever-wait #%d from %p %s`%s+%lu\n", logged, caller, base ? base + 1 : di.dli_fname,
		    di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? (unsigned long)((uintptr_t)caller - (uintptr_t)di.dli_saddr) : 0UL);
	} else {
		n = snprintf(b, sizeof b, "semfast: forever-wait #%d from %p\n", logged, caller);
	}
	(void)write(con, b, (size_t)n);
}

static long
my_semaphore_wait(dispatch_semaphore_t sem, dispatch_time_t timeout)
{
	if (timeout == DISPATCH_TIME_FOREVER) {
		note(__builtin_return_address(0));
		timeout = dispatch_time(DISPATCH_TIME_NOW, SEM_SECONDS * (int64_t)NSEC_PER_SEC);
	}
	return dispatch_semaphore_wait(sem, timeout);
}

#define INTERPOSE(new, old) { (const void *)(uintptr_t)(new), (const void *)(uintptr_t)(old) }
__attribute__((used)) static const struct {
	const void *replacement, *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	INTERPOSE(my_semaphore_wait, dispatch_semaphore_wait),
};
