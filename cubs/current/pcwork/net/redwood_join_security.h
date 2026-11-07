/* Fail-closed classification of a scanned BSS for the experimental join path. */
#ifndef CUBS_JOIN_SECURITY_H
#define CUBS_JOIN_SECURITY_H
#include <stddef.h>
#include <stdint.h>

enum cubs_join_security { CUBS_JOIN_OPEN, CUBS_JOIN_WPA2_PSK, CUBS_JOIN_WPA3_SAE, CUBS_JOIN_MIXED, CUBS_JOIN_UNSUPPORTED };
/* OpenIntelWireless apple80211_var.h: OPEN=1, upper NONE=0, WPA2_PSK=1<<3.
 * Only exact, explicitly understood modes are accepted. */
static int cubs_join_auth_matches(enum cubs_join_security mode,unsigned lower,unsigned upper) {
    if (lower!=1) return 0;
    if (mode==CUBS_JOIN_OPEN) return upper==0;
    if (mode==CUBS_JOIN_WPA2_PSK) return upper==(1u<<3);
    return 0;
}
static unsigned cubs_rsn_u16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1]<<8; }

static int cubs_rsn_modes(const uint8_t *p,size_t n) {
    if (n<8 || cubs_rsn_u16(p)!=1) return -1;
    size_t pos=6;
    unsigned count=cubs_rsn_u16(p+pos); pos+=2;
    if (!count || count>(n-pos)/4) return -1;
    pos+=(size_t)count*4;
    if (n-pos<2) return -1;
    count=cubs_rsn_u16(p+pos); pos+=2;
    if (!count || count>(n-pos)/4) return -1;
    int modes=0;
    for (unsigned i=0;i<count;i++,pos+=4) {
        if (p[pos] || p[pos+1]!=0x0f || p[pos+2]!=0xac) { modes|=4; continue; }
        if (p[pos+3]==2) modes|=1;
        else if (p[pos+3]==8) modes|=2;
        else modes|=4;
    }
    return modes;
}

static enum cubs_join_security cubs_join_security(unsigned capability,const uint8_t *ies,size_t n) {
    if (!ies && n) return CUBS_JOIN_UNSUPPORTED;
    unsigned modes=0; int saw_rsn=0, saw_wpa=0;
    for (size_t i=0;i<n;) {
        if (n-i<2 || (size_t)ies[i+1]>n-i-2) return CUBS_JOIN_UNSUPPORTED;
        unsigned id=ies[i],len=ies[i+1]; const uint8_t *p=ies+i+2;
        if (id==221 && len>=4 && p[0]==0 && p[1]==0x50 && p[2]==0xf2 && p[3]==1) saw_wpa=1;
        if (id==48) { int v=cubs_rsn_modes(p,len); if (v<0) return CUBS_JOIN_UNSUPPORTED; saw_rsn=1; modes|=(unsigned)v; }
        i+=2+len;
    }
    if (!saw_rsn) return !(capability&0x10) && !saw_wpa ? CUBS_JOIN_OPEN : CUBS_JOIN_UNSUPPORTED;
    if (!(capability&0x10) || (modes&4) || !modes) return CUBS_JOIN_UNSUPPORTED;
    if (modes==1) return CUBS_JOIN_WPA2_PSK;
    if (modes==2) return CUBS_JOIN_WPA3_SAE;
    return CUBS_JOIN_MIXED;
}
#endif
