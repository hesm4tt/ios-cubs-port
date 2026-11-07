/* Bounded decoder for SCN1 scan snapshots; no platform headers required. */
#ifndef CUBS_SCAN_DECODE_H
#define CUBS_SCAN_DECODE_H
#include <stdint.h>
#include <stddef.h>

struct cubs_scan_record {
    const uint8_t *bssid, *ssid, *elements;
    uint16_t ssid_length, elements_length, capability;
    uint32_t frequency, age_ms;
    int32_t rssi;
};
static uint32_t cubs_scan_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint16_t cubs_scan_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static int cubs_scan_parse(const uint8_t *p, size_t n,
    void (*each)(const struct cubs_scan_record *, void *), void *context,
    uint32_t *fresh_out, uint32_t *count_out) {
    if (!p || n < 16 || n > 16u+64u*(28u+32u+1024u) || cubs_scan_u32(p)!=0x314e4353u ||
        cubs_scan_u32(p+4)!=1 || cubs_scan_u32(p+8)>1 || cubs_scan_u32(p+12)>64) return -1;
    uint32_t count=cubs_scan_u32(p+12);
    /* Validate everything before delivering any records. */
    for (int pass=0; pass<2; pass++) {
        size_t off=16;
        for (uint32_t i=0; i<count; i++) {
            if (n-off < 28) return -1;
            uint32_t len=cubs_scan_u32(p+off), freq=cubs_scan_u32(p+off+4), age=cubs_scan_u32(p+off+12);
            int32_t rssi=(int32_t)cubs_scan_u32(p+off+8);
            uint16_t slen=cubs_scan_u16(p+off+16), ilen=cubs_scan_u16(p+off+18);
            if (slen>32 || ilen>1024 || (cubs_scan_u16(p+off+26) & ~0x13u) || len!=28u+slen+ilen || len>n-off || freq<2400 || freq>7125 ||
                rssi < -127 || rssi > 0 || age>0x7fffffffU) return -1;
            size_t e=off+28+slen, end=e+ilen;
            while (e<end) { if (end-e<2 || 2u+p[e+1]>end-e) return -1; e+=2u+p[e+1]; }
            if (pass==1 && each) {
                struct cubs_scan_record r={p+off+20,p+off+28,p+off+28+slen,slen,ilen,cubs_scan_u16(p+off+26),freq,age,rssi};
                each(&r,context);
            }
            off+=len;
        }
        if (off!=n) return -1;
    }
    if (fresh_out) *fresh_out=cubs_scan_u32(p+8);
    if (count_out) *count_out=count;
    return 0;
}
#endif
