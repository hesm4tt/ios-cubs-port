package com.cubs.cam;

/** Classifies advertised security without inferring WPA2 from a password alone. */
final class WifiSecurityClassifier {
    static final int OPEN=0, WPA2_PSK=1, WPA3_SAE=2, MIXED=3, UNSUPPORTED=4;

    static int classify(int capability, byte[] ies) {
        if (ies == null) return UNSUPPORTED;
        int modes=0; boolean rsn=false, vendorWpa=false;
        for (int i=0; i<ies.length;) {
            if (i+2>ies.length) return UNSUPPORTED;
            int id=ies[i]&255, len=ies[i+1]&255, start=i+2, end=start+len;
            if (end>ies.length) return UNSUPPORTED;
            if (id==221 && len>=4 && ies[start]==0 && (ies[start+1]&255)==0x50 &&
                (ies[start+2]&255)==0xf2 && (ies[start+3]&255)==1) vendorWpa=true;
            if (id==48) {
                rsn=true;
                int mask=rsnModes(ies,start,end);
                if (mask<0) return UNSUPPORTED;
                modes |= mask;
            }
            i=end;
        }
        if (!rsn) return ((capability & 0x10)==0 && !vendorWpa) ? OPEN : UNSUPPORTED;
        if ((capability & 0x10)==0) return UNSUPPORTED;
        if ((modes & 4)!=0 || modes==0) return UNSUPPORTED;
        if (modes==1) return WPA2_PSK;
        if (modes==2) return WPA3_SAE;
        return MIXED;
    }

    private static int u16(byte[] b,int p) { return (b[p]&255)|((b[p+1]&255)<<8); }
    private static int rsnModes(byte[] b,int p,int end) {
        if (end-p<8 || u16(b,p)!=1) return -1;
        p+=2+4; // version and group cipher
        int pairwise=u16(b,p); p+=2;
        if (pairwise==0 || pairwise>(end-p)/4) return -1;
        p+=pairwise*4;
        if (end-p<2) return -1;
        int count=u16(b,p); p+=2;
        if (count==0 || count>(end-p)/4) return -1;
        int modes=0;
        for (int i=0; i<count; i++,p+=4) {
            if (b[p]!=0 || (b[p+1]&255)!=0x0f || (b[p+2]&255)!=0xac) { modes|=4; continue; }
            int suite=b[p+3]&255;
            if (suite==2) modes|=1;
            else if (suite==8) modes|=2;
            else modes|=4;
        }
        return modes;
    }
}
