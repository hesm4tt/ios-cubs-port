package com.cubs.cam;

import android.net.wifi.IActionListener;
import android.net.wifi.IWifiManager;
import android.net.wifi.WifiConfiguration;
import android.os.IBinder;
import android.os.ServiceManager;
import java.io.DataInputStream;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;

/** Experimental one-shot root helper. Its caller must authenticate the stdin writer.
 * Nothing from the request belongs in arguments, environment, files or logs.
 * An accepted Binder request is not proof of association or internet access. */
public final class PrivilegedWifiJoin {
    private static final char[] HEX = "0123456789abcdef".toCharArray();

    private static String hex(byte[] bytes) {
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) { int n = bytes[i] & 255; out[2*i] = HEX[n >>> 4]; out[2*i+1] = HEX[n & 15]; }
        return new String(out);
    }

    static byte[] derivePmk(char[] password, byte[] ssid) throws Exception {
        PBEKeySpec spec = new PBEKeySpec(password, ssid, 4096, 256);
        try { return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA1").generateSecret(spec).getEncoded(); }
        finally { spec.clearPassword(); }
    }

    static String saePassword(byte[] key) {
        if (key.length < 1 || key.length > 63) throw new IllegalArgumentException();
        char[] password = new char[key.length];
        try {
            for (int i=0; i<key.length; i++) {
                int c=key[i]&255;
                if (c<32 || c>126) throw new IllegalArgumentException();
                password[i]=(char)c;
            }
            return "\"" + new String(password) + "\"";
        } finally { Arrays.fill(password,'\0'); }
    }

    public static void main(String[] args) {
        byte[] ssid = null, bssid = null, key = null, pmk = null;
        char[] password = null;
        try {
            DataInputStream input = new DataInputStream(System.in);
            byte[] header = new byte[8]; input.readFully(header);
            int ssidLen = header[4] & 255, mode = header[5] & 255, bssidLen = header[6] & 255, keyLen = header[7] & 255;
            if (header[0]!='J' || header[1]!='O' || header[2]!='N' || header[3]!='1' || ssidLen<1 || ssidLen>32 ||
                (bssidLen!=0 && bssidLen!=6) || mode>3 ||
                (mode==0 && keyLen!=0) || (mode==1 && (keyLen<8 || keyLen>63)) || (mode==2 && keyLen!=32) ||
                (mode==3 && (keyLen<1 || keyLen>63))) {
                System.out.println("request-invalid"); return;
            }
            ssid = new byte[ssidLen]; bssid = new byte[bssidLen]; key = new byte[keyLen];
            input.readFully(ssid); input.readFully(bssid); input.readFully(key);
            WifiConfiguration config = new WifiConfiguration();
            config.SSID = hex(ssid); // Android accepts an unquoted raw-SSID hex string.
            if (bssidLen == 6) {
                String h = hex(bssid);
                config.BSSID = h.substring(0,2)+":"+h.substring(2,4)+":"+h.substring(4,6)+":"+
                    h.substring(6,8)+":"+h.substring(8,10)+":"+h.substring(10,12);
            }
            if (mode == 0) config.setSecurityParams(WifiConfiguration.SECURITY_TYPE_OPEN);
            else if (mode == 3) {
                // AOSP's supplicant path requires a quoted SAE passphrase, not a WPA2 PMK.
                // Restrict this prototype to printable ASCII until end-to-end encoding is verified.
                config.setSecurityParams(WifiConfiguration.SECURITY_TYPE_SAE);
                config.preSharedKey=saePassword(key);
            } else {
                if (mode == 1) {
                    password = new char[key.length];
                    for (int i=0; i<key.length; i++) {
                        int c=key[i]&255;
                        if (c<32 || c>126) { System.out.println("request-invalid"); return; }
                        password[i]=(char)c;
                    }
                    pmk = derivePmk(password, ssid);
                } else pmk=key.clone();
                config.setSecurityParams(WifiConfiguration.SECURITY_TYPE_PSK);
                config.preSharedKey=hex(pmk);
            }
            IBinder binder = ServiceManager.checkService("wifi");
            if (binder == null) { System.out.println("wifi-service-unavailable"); return; }
            CountDownLatch done = new CountDownLatch(1);
            IActionListener listener = new IActionListener.Stub() {
                @Override public void onSuccess() { System.out.println("request-accepted"); done.countDown(); }
                @Override public void onFailure(int reason) { System.out.println("request-rejected reason=" + reason); done.countDown(); }
            };
            IWifiManager.Stub.asInterface(binder).connect(config, -1, listener, "com.android.shell", null);
            if (!done.await(10, TimeUnit.SECONDS)) System.out.println("request-timeout");
            config.preSharedKey=null;
        } catch (Throwable ignored) {
            // Exception messages from platform services may contain network details.
            System.out.println("request-failed");
        } finally {
            if (ssid != null) Arrays.fill(ssid,(byte)0);
            if (bssid != null) Arrays.fill(bssid,(byte)0);
            if (key != null) Arrays.fill(key,(byte)0);
            if (pmk != null) Arrays.fill(pmk,(byte)0);
            if (password != null) Arrays.fill(password,'\0');
        }
    }
}
