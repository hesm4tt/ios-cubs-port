package com.cubs.cam;

import java.nio.charset.StandardCharsets;

public final class PrivilegedWifiJoinTest {
    public static void main(String[] args) throws Exception {
        byte[] pmk=PrivilegedWifiJoin.derivePmk("password".toCharArray(), "IEEE".getBytes(StandardCharsets.US_ASCII));
        StringBuilder s=new StringBuilder();
        for (byte b:pmk) s.append(String.format("%02x",b & 255));
        if (!s.toString().equals("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e")) throw new AssertionError("WPA2 test vector");
        if (!PrivilegedWifiJoin.saePassword("a\"\\z".getBytes(StandardCharsets.US_ASCII)).equals("\"a\"\\z\"")) throw new AssertionError("SAE quoting");
        try { PrivilegedWifiJoin.saePassword(new byte[]{0}); throw new AssertionError("invalid SAE accepted"); }
        catch (IllegalArgumentException expected) { }
        System.out.println("WPA2 derivation test passed");
        System.out.println("SAE formatting test passed");
    }
}
