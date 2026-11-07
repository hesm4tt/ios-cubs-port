/*
 * libmgfast.dylib: for processes (lsd) that would wait ~16 minutes in libMobileGestalt for an answer that does not come
 * on this port during first boot (SpringBoard waits for lsd), bound every MobileGestalt question to MGFAST_TIMEOUT seconds:
 * the real call runs on a worker thread, and when it has not answered in time the caller gets "no answer" (NULL / 0 / default).
 * Loaded through the kernel's exec-inject list (INJECT in build.sh); interposes the question functions of libMobileGestalt.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define MGFAST_TIMEOUT 2

extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern CFTypeRef MGCopyAnswerWithError(CFStringRef key, CFDictionaryRef options, int *error);
extern bool MGGetBoolAnswer(CFStringRef key);
extern int32_t MGGetSInt32Answer(CFStringRef key, int32_t def);
extern int64_t MGGetSInt64Answer(CFStringRef key, int64_t def);

static int con = -2;
static int timeouts;

static void
note(const char *what)
{
	char b[120];

	if (con == -2) {
		con = open("/dev/console", O_WRONLY);
	}
	if (con >= 0 && timeouts < 40) {
		int n = snprintf(b, sizeof b, "mgfast: %s timed out (%d)\n", what, ++timeouts);

		(void)write(con, b, (size_t)n);
	}
}

/* run block on a worker thread; returns 1 when it finished within the timeout */
static int
timed(void (^work)(void))
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		work();
		dispatch_semaphore_signal(done);
	});
	return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, MGFAST_TIMEOUT * NSEC_PER_SEC)) == 0;
}

static CFTypeRef
my_MGCopyAnswer(CFStringRef key)
{
	__block CFTypeRef r = NULL;

	if (!timed(^{ r = MGCopyAnswer(key); })) {
		note("MGCopyAnswer");
		return NULL;
	}
	return r;
}

static CFTypeRef
my_MGCopyAnswerWithError(CFStringRef key, CFDictionaryRef options, int *error)
{
	__block CFTypeRef r = NULL;
	__block int e = 0;

	if (!timed(^{ r = MGCopyAnswerWithError(key, options, &e); })) {
		note("MGCopyAnswerWithError");
		if (error) {
			*error = 1;
		}
		return NULL;
	}
	if (error) {
		*error = e;
	}
	return r;
}

static bool
my_MGGetBoolAnswer(CFStringRef key)
{
	__block bool r = false;

	if (!timed(^{ r = MGGetBoolAnswer(key); })) {
		note("MGGetBoolAnswer");
		return false;
	}
	return r;
}

static int32_t
my_MGGetSInt32Answer(CFStringRef key, int32_t def)
{
	__block int32_t r = def;

	if (!timed(^{ r = MGGetSInt32Answer(key, def); })) {
		note("MGGetSInt32Answer");
		return def;
	}
	return r;
}

static int64_t
my_MGGetSInt64Answer(CFStringRef key, int64_t def)
{
	__block int64_t r = def;

	if (!timed(^{ r = MGGetSInt64Answer(key, def); })) {
		note("MGGetSInt64Answer");
		return def;
	}
	return r;
}

#define INTERPOSE(new, old) { (const void *)(uintptr_t)(new), (const void *)(uintptr_t)(old) }
__attribute__((used)) static const struct {
	const void *replacement, *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	INTERPOSE(my_MGCopyAnswer, MGCopyAnswer),
	INTERPOSE(my_MGCopyAnswerWithError, MGCopyAnswerWithError),
	INTERPOSE(my_MGGetBoolAnswer, MGGetBoolAnswer),
	INTERPOSE(my_MGGetSInt32Answer, MGGetSInt32Answer),
	INTERPOSE(my_MGGetSInt64Answer, MGGetSInt64Answer),
};
