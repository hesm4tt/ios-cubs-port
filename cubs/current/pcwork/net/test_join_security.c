#include <stdio.h>
#include "redwood_join_security.h"
int main(void) {
    uint8_t p[]={48,18,1,0, 0,15,172,4, 1,0, 0,15,172,4, 1,0, 0,15,172,2};
    uint8_t malformed[]={48,3,1};
    if (cubs_join_security(0,NULL,0)!=CUBS_JOIN_OPEN ||
        cubs_join_security(16,NULL,0)!=CUBS_JOIN_UNSUPPORTED ||
        cubs_join_security(16,p,sizeof(p))!=CUBS_JOIN_WPA2_PSK) return 1;
    p[19]=8;
    if (cubs_join_security(16,p,sizeof(p))!=CUBS_JOIN_WPA3_SAE) return 2;
    p[19]=1;
    if (cubs_join_security(16,p,sizeof(p))!=CUBS_JOIN_UNSUPPORTED ||
        cubs_join_security(16,malformed,sizeof(malformed))!=CUBS_JOIN_UNSUPPORTED) return 3;
    uint8_t mixed[]={48,22,1,0, 0,15,172,4, 1,0, 0,15,172,4, 2,0, 0,15,172,2, 0,15,172,8};
    if (cubs_join_security(16,mixed,sizeof(mixed))!=CUBS_JOIN_MIXED) return 4;
    if (!cubs_join_auth_matches(CUBS_JOIN_OPEN,1,0) || !cubs_join_auth_matches(CUBS_JOIN_WPA2_PSK,1,8) ||
        cubs_join_auth_matches(CUBS_JOIN_WPA2_PSK,1,0x1000) || cubs_join_auth_matches(CUBS_JOIN_WPA2_PSK,1,0x1008) ||
        cubs_join_auth_matches(CUBS_JOIN_WPA3_SAE,1,0x1000) || cubs_join_auth_matches(CUBS_JOIN_OPEN,2,0)) return 5;
    puts("join security checks passed");
    return 0;
}
