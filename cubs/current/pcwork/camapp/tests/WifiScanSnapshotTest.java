package com.cubs.cam;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

public final class WifiScanSnapshotTest {
    public static void main(String[] args) {
        byte[] ap = { 0, 1, 2, 3, 4, 5 }, name = { 65, 66 }, rsn = { 48, 2, 1, 0 };
        WifiScanSnapshot.Record good = new WifiScanSnapshot.Record(ap, name, rsn, 5180, -55, 42);
        WifiScanSnapshot.Record bad = new WifiScanSnapshot.Record(ap, name, new byte[]{48, 3, 1}, 5180, -55, 42);
        ByteBuffer b = ByteBuffer.wrap(WifiScanSnapshot.encode(Arrays.asList(good, bad), false)).order(ByteOrder.LITTLE_ENDIAN);
        if (b.getInt() != WifiScanSnapshot.MAGIC || b.getInt() != 1 || b.getInt() != 0 || b.getInt() != 1) throw new AssertionError("header");
        if (b.getInt() != 34 || b.getInt() != 5180 || b.getInt() != -55 || b.getInt() != 42) throw new AssertionError("record");
        if (b.getShort() != 2 || b.getShort() != 4) throw new AssertionError("lengths");
        byte[] address = new byte[6]; b.get(address);
        if (!Arrays.equals(address, ap) || b.getShort() != 0) throw new AssertionError("address");
        byte[] payload = new byte[6]; b.get(payload);
        if (!Arrays.equals(payload, new byte[]{65, 66, 48, 2, 1, 0}) || b.hasRemaining()) throw new AssertionError("payload");
        System.out.println("scan snapshot validation passed");
    }
}
