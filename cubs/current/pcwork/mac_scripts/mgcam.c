/*
 * libmgcam.dylib: SpringBoard, lsd and Camera.app ask MobileGestalt whether the device has a camera (UIRequiredDeviceCapabilities: still-camera ...); on this port
 * the answer is false (no IORegistry camera service), so Camera.app has no icon. The Pixel's cameras are there through the bridge (redwood_camera.c), so say yes.
 * Loaded through the kernel's exec-inject list; interposes MGGetBoolAnswer and MGCopyAnswer.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern bool MGGetBoolAnswer(CFStringRef key);

static int
is_cam(CFStringRef k)
{
	static const char *const names[] = { "still-camera", "rear-facing-camera", "front-facing-camera", "camera-flash", "video-camera", "auto-focus-camera", "front-camera-flash",
		                             "camera-front-flash", "hi-res-camera", "camera-rear-flash", NULL };
	char b[64];

	if (k == NULL || !CFStringGetCString(k, b, sizeof b, kCFStringEncodingUTF8)) {
		return 0;
	}
	for (int i = 0; names[i]; i++) {
		if (strcmp(b, names[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static bool
my_MGGetBoolAnswer(CFStringRef key)
{
	return is_cam(key) ? true : MGGetBoolAnswer(key);
}

static CFTypeRef
my_MGCopyAnswer(CFStringRef key)
{
	return is_cam(key) ? (CFTypeRef)CFRetain(kCFBooleanTrue) : MGCopyAnswer(key);
}

#define INTERPOSE(new, old) { (const void *)(uintptr_t)(new), (const void *)(uintptr_t)(old) }
__attribute__((used)) static const struct {
	const void *replacement, *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	INTERPOSE(my_MGCopyAnswer, MGCopyAnswer),
	INTERPOSE(my_MGGetBoolAnswer, MGGetBoolAnswer),
};
