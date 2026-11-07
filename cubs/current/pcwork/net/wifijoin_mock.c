/* Test-only stand-in: validates a fixed synthetic request and does no I/O to Wi-Fi. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    unsigned char data[12]; size_t n=0;
    while (n<sizeof(data)) { ssize_t r=read(0,data+n,sizeof(data)-n); if (r<=0) return 1; n+=(size_t)r; }
    const unsigned char expected[12]={'J','O','N','1',4,0,0,0,'T','E','S','T'};
    if (memcmp(data,expected,sizeof(data))) return 1;
    const char *marker=getenv("CUBS_JOIN_MOCK_MARKER");
    if (marker) { FILE *f=fopen(marker,"w"); if (!f) return 1; fputs("called\n",f); fclose(f); }
    puts("request-accepted");
    return 0;
}
