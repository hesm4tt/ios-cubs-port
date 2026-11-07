package com.cubs.cam;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

/** Bounded scan records for the experimental Android-to-iOS Wi-Fi bridge. */
final class WifiScanSnapshot {
    static final int MAGIC = 0x314e4353; // SCN1, little endian
    static final int MAX_RECORDS = 64, MAX_IES = 1024;

    static final class Record {
        final byte[] bssid, ssid, ies;
        final int frequency, rssi, ageMillis, capability;
        Record(byte[] bssid, byte[] ssid, byte[] ies, int frequency, int rssi, int ageMillis) {
            this(bssid, ssid, ies, frequency, rssi, ageMillis, 0);
        }
        Record(byte[] bssid, byte[] ssid, byte[] ies, int frequency, int rssi, int ageMillis, int capability) {
            this.bssid = bssid; this.ssid = ssid; this.ies = ies;
            this.frequency = frequency; this.rssi = rssi; this.ageMillis = ageMillis; this.capability = capability;
        }
    }

    /** Reject malformed records instead of inventing AP identity or security data. */
    static byte[] encode(List<Record> records, boolean fresh) {
        List<Record> accepted = new ArrayList<>();
        int size = 16;
        for (Record r : records) {
            if (r == null || r.bssid == null || r.bssid.length != 6 || r.ssid == null || r.ssid.length > 32 ||
                r.ies == null || r.ies.length > MAX_IES || r.frequency < 2400 || r.frequency > 7125 ||
                r.rssi < -127 || r.rssi > 0 || r.ageMillis < 0 || (r.capability & ~0x13) != 0 || !validElements(r.ies)) continue;
            accepted.add(r);
            size += 28 + r.ssid.length + r.ies.length;
            if (accepted.size() == MAX_RECORDS) break;
        }
        ByteBuffer out = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        out.putInt(MAGIC).putInt(1).putInt(fresh ? 1 : 0).putInt(accepted.size());
        for (Record r : accepted) {
            out.putInt(28 + r.ssid.length + r.ies.length).putInt(r.frequency).putInt(r.rssi).putInt(r.ageMillis);
            out.putShort((short)r.ssid.length).putShort((short)r.ies.length).put(r.bssid).putShort((short)r.capability);
            out.put(r.ssid).put(r.ies);
        }
        return out.array();
    }

    private static boolean validElements(byte[] ies) {
        for (int i = 0; i < ies.length;) {
            if (i + 2 > ies.length) return false;
            int length = ies[i + 1] & 0xff;
            i += 2 + length;
            if (i > ies.length) return false;
        }
        return true;
    }
}
