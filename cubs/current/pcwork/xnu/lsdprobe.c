/*
 * liblsdprobe.dylib: injected into lsd. Its thread asks MobileGestalt a list of common questions, each in a worker thread with a
 * 3 second timeout, and prints to /dev/console which answer in which time -- to find the question that never comes during iOS
 * first boot (lsd blocks in libMobileGestalt, SpringBoard waits for lsd).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

static int con = -1;

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void
say(const char *fmt, const char *a, double ms, const char *b)
{
	char buf[300];
	int n = snprintf(buf, sizeof buf, fmt, a, ms, b);

	if (con >= 0 && n > 0) {
		(void)write(con, buf, (size_t)n);
	}
}

static const char *const keys[] = {
	"ProductType", "ProductVersion", "BuildVersion", "DeviceClass", "DeviceName", "UniqueDeviceID", "SerialNumber",
	"InternalBuild", "HasBaseband", "RegionCode", "RegionInfo", "ChipID", "HardwarePlatform", "DeviceColor",
	"main-screen-scale", "main-screen-width", "main-screen-height", "SupportedDeviceFamilies", "DeviceSupportsApplePencil",
	"HWModelStr", "ModelNumber", "CPUArchitecture", "PhysicalHardwareNameString", "MarketingName", "ProductName",
	"DeviceEnclosureColor", "BluetoothAddress", "WifiAddress", "DieID", "BasebandVersion", "FirmwareVersion",
	"IsEmulatedDevice", "IsSimulator", "SIMStatus", "CarrierInstallCapability", "DeviceVariant", "ApNonce",
	"UserAssignedDeviceName", "AllDeviceCapabilities", "DeviceSupports1080p", "DeviceSupports3DMaps", "hall-effect-sensor",
	"gps", "wifi", "telephony", "metal", "opengles-2", "arm64", "armv7", "ipad", "fast-boot-time", "HasSEP", "SecureElement",
	"ArtworkTraits", "ArtworkDeviceSubType", "ArtworkDeviceIdiom", "BootSessionID", "TimeSinceBoot", "SupportsForceTouch",
	"DeviceSupportsFaceID", "HasExternalChargeBattery", "kMGQBasebandChipId", "oPeik/9e8lQWMszEjbPzng", NULL,
};

static void *
probe(void *arg)
{
	(void)arg;
	con = open("/dev/console", O_WRONLY);
	sleep(20);
	for (int round = 0; round < 3; round++) {
		say("lsdprobe: ===== round %s%.0f%s =====\n", "", (double)round, "");
		for (int i = 0; keys[i]; i++) {
			CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
			dispatch_semaphore_t done = dispatch_semaphore_create(0);
			__block int got = 0;
			double t0 = now();

			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				CFTypeRef a = MGCopyAnswer(k);

				got = a != NULL;
				if (a) {
					CFRelease(a);
				}
				dispatch_semaphore_signal(done);
			});
			int ok = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0;

			say("lsdprobe: %s %.0f ms %s\n", keys[i], (now() - t0) * 1000, ok ? (got ? "answered" : "no answer") : "TIMEOUT");
		}
		sleep(100);
	}
	return NULL;
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t t;

	if (pthread_create(&t, NULL, probe, NULL) == 0) {
		pthread_detach(t);
	}
}
