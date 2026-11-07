package com.cubs.cam;

import android.net.wifi.ScanResult;
import android.net.wifi.WifiSsid;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/** Converts Android scan results without logging network identifiers. */
final class WifiScanCollector {
    static byte[] encode(List<ScanResult> results, long elapsedRealtimeMicros, boolean fresh) {
        List<WifiScanSnapshot.Record> records = new ArrayList<>();
        if (results == null) return WifiScanSnapshot.encode(records, false);
        for (ScanResult result : results) {
            if (result == null || result.BSSID == null || result.timestamp <= 0 || result.timestamp > elapsedRealtimeMicros) continue;
            byte[] bssid = address(result.BSSID);
            WifiSsid wifiSsid = result.getWifiSsid();
            if (bssid == null || wifiSsid == null) continue;
            byte[] ssid = wifiSsid.getBytes();
            if (ssid == null || ssid.length > 32) continue;
            ByteArrayOutputStream ies = new ByteArrayOutputStream();
            List<ScanResult.InformationElement> elements = result.getInformationElements();
            if (elements != null) for (ScanResult.InformationElement element : elements) {
                int id = element.getId();
                ByteBuffer bytes = element.getBytes().duplicate();
                int length = bytes.remaining();
                if (id < 0 || id > 255 || length > 255 || ies.size() + length + 2 > WifiScanSnapshot.MAX_IES) continue;
                ies.write(id); ies.write(length);
                while (bytes.hasRemaining()) ies.write(bytes.get() & 0xff);
            }
            long age = (elapsedRealtimeMicros - result.timestamp) / 1000;
            int[] security = result.getSecurityTypes();
            int capability = 0x10; // Unknown security fails closed.
            if (security != null && security.length == 1 && security[0] == 0) capability = 0;
            if (result.capabilities != null && result.capabilities.contains("[ESS]")) capability |= 0x01;
            if (result.capabilities != null && result.capabilities.contains("[IBSS]")) capability |= 0x02;
            records.add(new WifiScanSnapshot.Record(bssid, ssid, ies.toByteArray(), result.frequency,
                result.level, (int)Math.min(age, Integer.MAX_VALUE), capability));
            if (records.size() == WifiScanSnapshot.MAX_RECORDS) break;
        }
        return WifiScanSnapshot.encode(records, fresh);
    }

    private static byte[] address(String text) {
        if (text.length() != 17) return null;
        byte[] out = new byte[6];
        for (int i = 0; i < 6; i++) {
            int p = i * 3;
            if (i != 5 && text.charAt(p + 2) != ':') return null;
            int hi = Character.digit(text.charAt(p), 16), lo = Character.digit(text.charAt(p + 1), 16);
            if (hi < 0 || lo < 0) return null;
            out[i] = (byte)((hi << 4) | lo);
        }
        return out;
    }
}
