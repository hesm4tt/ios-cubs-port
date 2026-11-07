#include <stdio.h>
#include <string.h>
#include "redwood_scan_decode.h"
static void got(const struct cubs_scan_record *r, void *context) {
    int *count=context;
    if (r->frequency!=5180 || r->rssi!=-55 || r->age_ms!=42 || r->ssid_length!=2 ||
        memcmp(r->ssid,"AB",2) || r->elements_length!=4 || r->elements[0]!=48) *count=-100;
    else (*count)++;
}
int main(void) {
    unsigned char p[]={ 'S','C','N','1', 1,0,0,0, 1,0,0,0, 1,0,0,0,
        34,0,0,0, 60,20,0,0, 201,255,255,255, 42,0,0,0,
        2,0,4,0, 0,1,2,3,4,5,0,0, 'A','B',48,2,1,0 };
    uint32_t fresh=0, count=0; int seen=0;
    if (cubs_scan_parse(p,sizeof(p),got,&seen,&fresh,&count) || seen!=1 || fresh!=1 || count!=1) return 1;
    p[sizeof(p)-3]=3; seen=0;
    if (!cubs_scan_parse(p,sizeof(p),got,&seen,&fresh,&count) || seen) return 2;
    puts("scan decode checks passed");
    return 0;
}
