package com.cubs.cam;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.Arrays;

/**
 * Streams the camera's frames (I420 planes: Y, then U, then V) to the daemon camsrv on 127.0.0.1:7777.
 * Frame header (little endian): magic "CAM1", seq, width, height, length. am start -n com.cubs.cam/.CamActivity --es id 0 --ei w 640 --ei h 480
 */
public class CamService extends Service {
    static final String T = "CubsCam";
    HandlerThread ht;
    Handler h;
    CameraDevice dev;
    ImageReader rd;
    Socket sock;
    OutputStream out;
    int seq, w = 640, ht_ = 480;
    byte[] buf;

    @Override
    public IBinder onBind(Intent i) { return null; }

    @Override
    public int onStartCommand(Intent in, int flags, int startId) {
        if (dev != null) return START_STICKY;
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel("cam", "cam", NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, "cam").setContentTitle("camera bridge").setSmallIcon(android.R.drawable.ic_menu_camera).build();
        startForeground(7, n);
        String id = in != null ? in.getStringExtra("id") : null;
        if (id == null) id = "0";
        w = in != null ? in.getIntExtra("w", 640) : 640;
        ht_ = in != null ? in.getIntExtra("h", 480) : 480;
        ht = new HandlerThread("cam");
        ht.start();
        h = new Handler(ht.getLooper());
        final String cid = id;
        int delay = in != null ? in.getIntExtra("delay", 0) : 0;
        Log.i(T, "camera " + cid + " will be opened in " + delay + " s");
        h.postDelayed(new Runnable() {
            public void run() {
                try {
                    CameraManager m = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
                    Log.i(T, "opening camera " + cid + " " + w + "x" + ht_);
                    m.openCamera(cid, new CameraDevice.StateCallback() {
                        public void onOpened(CameraDevice d) { dev = d; start(); }
                        public void onDisconnected(CameraDevice d) { Log.e(T, "disconnected"); d.close(); }
                        public void onError(CameraDevice d, int e) { Log.e(T, "error " + e); d.close(); }
                    }, h);
                } catch (Exception e) {
                    Log.e(T, "open failed", e);
                }
            }
        }, delay * 1000L);
        return START_STICKY;
    }

    void start() {
        try {
            rd = ImageReader.newInstance(w, ht_, ImageFormat.YUV_420_888, 4);
            rd.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                public void onImageAvailable(ImageReader r) { frame(r); }
            }, h);
            final Surface s = rd.getSurface();
            dev.createCaptureSession(Arrays.asList(s), new CameraCaptureSession.StateCallback() {
                public void onConfigured(CameraCaptureSession ses) {
                    try {
                        CaptureRequest.Builder rb = dev.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        rb.addTarget(s);
                        ses.setRepeatingRequest(rb.build(), null, h);
                        Log.i(T, "streaming");
                    } catch (Exception e) { Log.e(T, "request failed", e); }
                }
                public void onConfigureFailed(CameraCaptureSession ses) { Log.e(T, "configure failed"); }
            }, h);
        } catch (Exception e) {
            Log.e(T, "start failed", e);
        }
    }

    void frame(ImageReader r) {
        Image im = r.acquireLatestImage();
        if (im == null) return;
        try {
            int W = im.getWidth(), H = im.getHeight();
            int len = W * H * 3 / 2;
            if (buf == null || buf.length != 20 + len) buf = new byte[20 + len];
            put(buf, 0, 0x31414d43);
            put(buf, 4, ++seq);
            put(buf, 8, W);
            put(buf, 12, H);
            put(buf, 16, len);
            Image.Plane[] p = im.getPlanes();
            int o = 20;
            for (int pi = 0; pi < 3; pi++) {
                int pw = pi == 0 ? W : W / 2, ph = pi == 0 ? H : H / 2;
                ByteBuffer bb = p[pi].getBuffer();
                int rs = p[pi].getRowStride(), ps = p[pi].getPixelStride();
                for (int y = 0; y < ph; y++) {
                    int base = y * rs;
                    if (ps == 1) {
                        bb.position(base);
                        bb.get(buf, o, pw);
                        o += pw;
                    } else {
                        for (int x = 0; x < pw; x++) buf[o++] = bb.get(base + x * ps);
                    }
                }
            }
            send(buf);
        } catch (Exception e) {
            Log.e(T, "frame failed", e);
        } finally {
            im.close();
        }
    }

    static void put(byte[] b, int o, int v) {
        b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); b[o + 2] = (byte) (v >> 16); b[o + 3] = (byte) (v >> 24);
    }

    void send(byte[] b) {
        try {
            if (sock == null) {
                sock = new Socket("127.0.0.1", 7777);
                out = sock.getOutputStream();
                Log.i(T, "connected to camsrv");
            }
            out.write(b);
            out.flush();
        } catch (Exception e) {
            Log.e(T, "send failed: " + e);
            try { if (sock != null) sock.close(); } catch (Exception x) { }
            sock = null;
        }
    }
}
