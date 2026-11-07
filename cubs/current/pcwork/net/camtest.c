/*
 * camtest: can a root process on Android take a picture with the camera? (Gate for the iOS camera bridge.)
 * Opens camera ID (argv[1], default the first the manager lists), reads one YUV_420_888 frame of 640x480 through an AImageReader and writes the Y plane
 * to /data/local/tmp/cam_y.raw. Prints every step and error code.
 */
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int got;
static void on_image(void *ctx, AImageReader *r)
{
	AImage *img = NULL;
	(void)ctx;
	if (AImageReader_acquireLatestImage(r, &img) != AMEDIA_OK || !img) {
		return;
	}
	uint8_t *data = NULL;
	int len = 0, rowstride = 0, w = 0, h = 0;

	AImage_getPlaneData(img, 0, &data, &len);
	AImage_getPlaneRowStride(img, 0, &rowstride);
	AImage_getWidth(img, &w);
	AImage_getHeight(img, &h);
	if (!got) {
		FILE *f = fopen("/data/local/tmp/cam_y.raw", "wb");

		if (f) {
			fwrite(data, 1, (size_t)len, f);
			fclose(f);
		}
		unsigned long sum = 0;

		for (int i = 0; i < len; i++) {
			sum += data[i];
		}
		printf("camtest: frame %dx%d, Y plane %d bytes, rowstride %d, byte sum %lu\n", w, h, len, rowstride, sum);
		got = 1;
	}
	AImage_delete(img);
}

static void d_disc(void *c, ACameraDevice *d) { (void)c; (void)d; printf("camtest: device disconnected\n"); }
static void d_err(void *c, ACameraDevice *d, int e) { (void)c; (void)d; printf("camtest: device error %d\n", e); }
static void s_closed(void *c, ACameraCaptureSession *s) { (void)c; (void)s; }
static void s_ready(void *c, ACameraCaptureSession *s) { (void)c; (void)s; }
static void s_active(void *c, ACameraCaptureSession *s) { (void)c; (void)s; }

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	ACameraManager *mgr = ACameraManager_create();
	ACameraIdList *ids = NULL;
	camera_status_t st = ACameraManager_getCameraIdList(mgr, &ids);

	printf("camtest: camera id list: status %d, %d cameras\n", st, ids ? ids->numCameras : -1);
	if (st != ACAMERA_OK || !ids || ids->numCameras < 1) {
		return 2;
	}
	for (int i = 0; i < ids->numCameras; i++) {
		printf("camtest:   id %s\n", ids->cameraIds[i]);
	}
	const char *id = argc > 1 ? argv[1] : ids->cameraIds[0];
	ACameraDevice_stateCallbacks dcb = { NULL, d_disc, d_err };
	ACameraDevice *dev = NULL;

	st = ACameraManager_openCamera(mgr, id, &dcb, &dev);
	printf("camtest: open %s: status %d\n", id, st);
	if (st != ACAMERA_OK) {
		return 3;
	}
	AImageReader *rd = NULL;
	int W = argc > 2 ? atoi(argv[2]) : 1280, H = argc > 3 ? atoi(argv[3]) : 720;
	media_status_t ms = AImageReader_newWithUsage(W, H, argc > 4 ? AIMAGE_FORMAT_JPEG : AIMAGE_FORMAT_YUV_420_888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 6, &rd);
	AImageReader_ImageListener lst = { NULL, on_image };

	printf("camtest: image reader: %d\n", ms);
	AImageReader_setImageListener(rd, &lst);
	ANativeWindow *win = NULL;

	AImageReader_getWindow(rd, &win);
	ACaptureSessionOutputContainer *oc = NULL;
	ACaptureSessionOutput *out = NULL;
	ACameraOutputTarget *tgt = NULL;
	ACaptureRequest *req = NULL;
	ACameraCaptureSession *ses = NULL;

	ACaptureSessionOutputContainer_create(&oc);
	ACaptureSessionOutput_create(win, &out);
	ACaptureSessionOutputContainer_add(oc, out);
	ACameraDevice_createCaptureRequest(dev, argc > 4 ? TEMPLATE_STILL_CAPTURE : TEMPLATE_PREVIEW, &req);
	ACameraOutputTarget_create(win, &tgt);
	ACaptureRequest_addTarget(req, tgt);
	ACameraCaptureSession_stateCallbacks scb = { NULL, s_closed, s_ready, s_active };

	st = ACameraDevice_createCaptureSession(dev, oc, &scb, &ses);
	printf("camtest: session: status %d\n", st);
	st = ACameraCaptureSession_setRepeatingRequest(ses, NULL, 1, &req, NULL);
	printf("camtest: repeating request: status %d\n", st);
	for (int i = 0; i < 200 && !got; i++) {
		on_image(NULL, rd);
		usleep(50000);
	}
	printf("camtest: %s\n", got ? "GOT A FRAME" : "NO FRAME in 10 s");
	return got ? 0 : 4;
}
