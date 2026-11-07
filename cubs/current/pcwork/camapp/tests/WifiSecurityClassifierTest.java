package com.cubs.cam;

public final class WifiSecurityClassifierTest {
    private static byte[] rsn(int... suites) {
        byte[] out=new byte[2+2+4+2+4+2+4*suites.length];
        out[0]=48; out[1]=(byte)(out.length-2); out[2]=1;
        out[4]=0; out[5]=15; out[6]=(byte)172; out[7]=4;
        out[8]=1; out[10]=0; out[11]=15; out[12]=(byte)172; out[13]=4;
        out[14]=(byte)suites.length;
        for (int i=0;i<suites.length;i++) { int p=16+4*i; out[p]=0; out[p+1]=15; out[p+2]=(byte)172; out[p+3]=(byte)suites[i]; }
        return out;
    }
    public static void main(String[] args) {
        if (WifiSecurityClassifier.classify(0,new byte[0])!=WifiSecurityClassifier.OPEN ||
            WifiSecurityClassifier.classify(0x10,new byte[0])!=WifiSecurityClassifier.UNSUPPORTED ||
            WifiSecurityClassifier.classify(0x10,rsn(2))!=WifiSecurityClassifier.WPA2_PSK ||
            WifiSecurityClassifier.classify(0x10,rsn(8))!=WifiSecurityClassifier.WPA3_SAE ||
            WifiSecurityClassifier.classify(0x10,rsn(2,8))!=WifiSecurityClassifier.MIXED ||
            WifiSecurityClassifier.classify(0x10,rsn(1))!=WifiSecurityClassifier.UNSUPPORTED ||
            WifiSecurityClassifier.classify(0x10,new byte[]{48,3,1})!=WifiSecurityClassifier.UNSUPPORTED)
            throw new AssertionError("security classification");
        System.out.println("security classification checks passed");
    }
}
