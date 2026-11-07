package com.cubs.cam;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import java.io.OutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Read-only Wi-Fi state; no credentials or network identifiers are logged. */
public final class WifiStatusService extends Service {
    private static final String TAG = "CubsWifi";
    private Thread worker;
    private Thread scanWorker;
    private Thread prefetchWorker;
    private volatile boolean running;
    private boolean reported;
    private byte[] cachedBssid, cachedElements;
    private int cachedSecurity = -1;
    private long cachedUntil;
    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (worker != null) return START_NOT_STICKY;
        NotificationManager nm = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel("wifi", "iOS Wi-Fi", NotificationManager.IMPORTANCE_LOW));
        startForeground(9, new Notification.Builder(this, "wifi").setContentTitle("iOS Wi-Fi status")
            .setSmallIcon(android.R.drawable.ic_menu_info_details).build());
        running = true;
        // The root socket is created only after iOS starts. Sample while Android's
        // display and Wi-Fi services are still fully up, before waiting for it.
        prefetchWorker = new Thread(() -> {
            for (int i = 0; running && i < 12; i++) {
                sample();
                try { Thread.sleep(250); } catch (InterruptedException e) { break; }
            }
        }, "wifi-prefetch");
        prefetchWorker.start();
        worker = new Thread(() -> runLoop(), "wifi-status");
        worker.start();
        if (intent != null && intent.getBooleanExtra("scans", false)) {
            scanWorker = new Thread(() -> scanLoop(), "wifi-scan-cache");
            scanWorker.start();
        }
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        running = false;
        if (worker != null) worker.interrupt();
        if (scanWorker != null) scanWorker.interrupt();
        if (prefetchWorker != null) prefetchWorker.interrupt();
        super.onDestroy();
    }

    private synchronized byte[] sample() {
        WifiManager wm = (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        boolean powered = wm != null && wm.isWifiEnabled();
        WifiInfo info = powered ? wm.getConnectionInfo() : null;
        byte[] name = new byte[0], mac = new byte[6], ies = new byte[1024];
        int freq = 0, rssi = 0, security = -1, used = 0;
        boolean valid = false, rsn = false;
        try {
            if (info != null && info.getBSSID() != null) {
                String ssid = info.getSSID();
                if (ssid != null && ssid.length() >= 2 && ssid.startsWith("\"") && ssid.endsWith("\""))
                    ssid = ssid.substring(1, ssid.length() - 1);
                if (ssid != null && !"<unknown ssid>".equals(ssid)) name = ssid.getBytes(StandardCharsets.UTF_8);
                String[] parts = info.getBSSID().split(":");
                if (parts.length == 6) for (int i = 0; i < 6; i++) mac[i] = (byte)Integer.parseInt(parts[i], 16);
                freq = info.getFrequency(); rssi = info.getRssi(); security = info.getCurrentSecurityType();
                List<ScanResult.InformationElement> elements = info.getInformationElements();
                if (elements == null || elements.isEmpty()) {
                    for (ScanResult result : wm.getScanResults()) {
                        if (info.getBSSID().equalsIgnoreCase(result.BSSID)) {
                            elements = result.getInformationElements();
                            break;
                        }
                    }
                }
                if (elements != null) for (ScanResult.InformationElement element : elements) {
                    int id = element.getId(); ByteBuffer bytes = element.getBytes().duplicate(); int n = bytes.remaining();
                    if (id < 0 || id > 255 || n > 255 || used + n + 2 > ies.length) continue;
                    ies[used++] = (byte)id; ies[used++] = (byte)n; bytes.get(ies, used, n); used += n;
                    if (id == 48) rsn = true;
                }
                boolean plausibleAddress = parts.length == 6 && !"02:00:00:00:00:00".equals(info.getBSSID());
                if (rsn && plausibleAddress && name.length > 0 && name.length <= 32) {
                    cachedBssid = mac.clone();
                    cachedElements = java.util.Arrays.copyOf(ies, used);
                    cachedSecurity = security;
                    cachedUntil = SystemClock.elapsedRealtime() + 60000;
                } else if (!rsn && plausibleAddress && security == cachedSecurity && cachedBssid != null &&
                    java.util.Arrays.equals(mac, cachedBssid) && SystemClock.elapsedRealtime() <= cachedUntil) {
                    used = cachedElements.length;
                    System.arraycopy(cachedElements, 0, ies, 0, used);
                    rsn = true;
                }
                valid = parts.length == 6 && !"02:00:00:00:00:00".equals(info.getBSSID()) && name.length > 0 && name.length <= 32 && freq >= 2400 && freq <= 7125 &&
                    security >= 0 && (security == 0 || security == 1 || rsn);
                if (!reported) {
                    Log.i(TAG, "status: name=" + (name.length > 0) + " address=" + (parts.length == 6) +
                        " frequency=" + (freq > 0) + " security=" + security + " elements=" + used + " rsn=" + rsn);
                    reported = true;
                }
            }
        } catch (RuntimeException e) { valid = false; }
        ByteBuffer out = ByteBuffer.allocate(1100).order(ByteOrder.LITTLE_ENDIAN);
        out.putInt(0x31464957).putInt(0).putInt(powered ? 1 : 0).putInt(valid ? 1 : 0);
        out.putInt(freq).putInt(rssi).putInt(valid ? name.length : 0).putInt(security).putInt(valid ? used : 0);
        out.put(mac); out.put(name, 0, valid ? name.length : 0); out.position(36 + 6 + 32);
        if (valid) out.put(ies, 0, used);
        return out.array();
    }

    private void runLoop() {
        while (running) {
            try (LocalSocket socket = new LocalSocket()) {
                socket.connect(new LocalSocketAddress("cubs_wifi_status", LocalSocketAddress.Namespace.ABSTRACT));
                // The abstract socket namespace is shared. Do not send network
                // identifiers to an untrusted process that bound the name first.
                if (socket.getPeerCredentials().getUid() != 0)
                    throw new SecurityException("untrusted Wi-Fi status peer");
                OutputStream out = socket.getOutputStream();
                Log.i(TAG, "status channel connected");
                while (running) { out.write(sample()); out.flush(); Thread.sleep(5000); }
            } catch (Exception e) {
                if (!running) break;
                try { Thread.sleep(2000); } catch (InterruptedException ignored) { }
            }
        }
    }

    /* Publish periodic snapshots and respond to authenticated scan requests. */
    private void scanLoop() {
        while (running) {
            final Object scanLock = new Object();
            final long[] generation = {0};
            BroadcastReceiver receiver = new BroadcastReceiver() {
                @Override public void onReceive(Context context, Intent intent) {
                    if (intent != null && intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false)) {
                        synchronized (scanLock) { generation[0]++; scanLock.notifyAll(); }
                    }
                }
            };
            boolean registered = false;
            try (LocalSocket socket = new LocalSocket()) {
                socket.connect(new LocalSocketAddress("cubs_wifi_scan", LocalSocketAddress.Namespace.ABSTRACT));
                if (socket.getPeerCredentials().getUid() != 0) throw new SecurityException("untrusted scan peer");
                OutputStream out = socket.getOutputStream();
                InputStream in = socket.getInputStream();
                WifiManager wm = (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                registerReceiver(receiver, new IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION));
                registered = true;
                long lastScanRequest = -45000;
                long nextPublish = 0;
                while (running) {
                    boolean fresh = false;
                    long now = SystemClock.elapsedRealtime();
                    boolean requested = false;
                    while (in.available() > 0) {
                        int command = in.read();
                        if (command < 0) throw new java.io.EOFException();
                        if (command == 'S') requested = true;
                    }
                    if (!requested && now < nextPublish) { Thread.sleep(1000); continue; }
                    // Do one startup scan for status metadata, then reserve the
                    // Android scan quota for explicit iOS requests.
                    if (wm != null && (lastScanRequest < 0 || (requested && now - lastScanRequest >= 30000))) {
                        lastScanRequest = now;
                        synchronized (scanLock) {
                            long before = generation[0];
                            if (wm.startScan()) {
                                long until = SystemClock.elapsedRealtime() + 10000;
                                while (running && generation[0] == before && SystemClock.elapsedRealtime() < until)
                                    scanLock.wait(Math.max(1, until - SystemClock.elapsedRealtime()));
                                fresh = generation[0] != before;
                            }
                        }
                    }
                    byte[] data = WifiScanCollector.encode(wm == null ? null : wm.getScanResults(),
                        SystemClock.elapsedRealtimeNanos() / 1000, fresh);
                    out.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(data.length).array());
                    out.write(data); out.flush();
                    nextPublish = SystemClock.elapsedRealtime() + 15000;
                }
            } catch (Exception e) {
                if (!running) break;
                try { Thread.sleep(2000); } catch (InterruptedException ignored) { }
            } finally {
                if (registered) unregisterReceiver(receiver);
            }
        }
    }
}
