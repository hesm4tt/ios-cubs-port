/* Receive cached Android Wi-Fi scan records from the authenticated app.
 * Experimental: no XNU consumer yet; never log network identifiers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define OFF 0x2100000
#define MAGIC 0x31424353u /* SCB1 */
#define SNAP_MAGIC 0x314e4353u /* SCN1 */
#define MAX_SIZE (16u + 64u * (28u + 32u + 1024u))
static int ring;
static uint32_t sequence;

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static int valid(const unsigned char *p, uint32_t n) {
    if (n < 16 || n > MAX_SIZE || u32(p) != SNAP_MAGIC || u32(p+4) != 1 || u32(p+8) > 1 || u32(p+12) > 64) return 0;
    uint32_t off = 16, count = u32(p+12);
    for (uint32_t i = 0; i < count; i++) {
        if (n - off < 28) return 0;
        uint32_t len = u32(p+off), freq = u32(p+off+4);
        int32_t rssi = (int32_t)u32(p+off+8);
        unsigned slen = u16(p+off+16), ilen = u16(p+off+18);
        if (len != 28u+slen+ilen || len > n-off || slen > 32 || ilen > 1024 || (u16(p+off+26) & ~0x13u) || freq < 2400 || freq > 7125 || rssi < -127 || rssi > 0 || (int32_t)u32(p+off+12) < 0) return 0;
        uint32_t e = off+28+slen, end = e+ilen;
        while (e < end) { if (end-e < 2 || 2u+p[e+1] > end-e) return 0; e += 2u+p[e+1]; }
        off += len;
    }
    return off == n;
}
static int publish(const unsigned char *p, uint32_t n) {
    uint32_t odd = sequence + 1, magic = MAGIC;
    if (pwrite(ring,&odd,4,OFF+4)!=4 || pwrite(ring,&magic,4,OFF)!=4 ||
        (n && pwrite(ring,p,n,OFF+0x100)!=(ssize_t)n) || pwrite(ring,&n,4,OFF+8)!=4) return -1;
    sequence += 2;
    return pwrite(ring,&sequence,4,OFF+4)==4 ? 0 : -1;
}
static int read_full(int fd, void *buf, size_t n) {
    size_t have=0;
    while (have<n) { ssize_t r=read(fd,(char *)buf+have,n-have); if (r<0 && errno==EINTR) continue; if (r<=0) return -1; have+=(size_t)r; }
    return 0;
}
int main(int argc, char **argv) {
    char *end=NULL;
    if (argc!=2) return 2;
    unsigned long uid=strtoul(argv[1],&end,10);
    if (!end || *end || uid<10000 || uid>999999) return 2;
    const char *path=getenv("CUBS_WIFI_RING");
    ring=open(path ? path : "/dev/cubs_ram",O_RDWR);
    if (ring<0) return 1;
    int ls=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    const char *name="cubs_wifi_scan";
    memcpy(addr.sun_path+1,name,strlen(name));
    if (ls<0 || bind(ls,(struct sockaddr *)&addr,offsetof(struct sockaddr_un,sun_path)+1+strlen(name)) || listen(ls,1)) return 1;
    unsigned char *buf=malloc(MAX_SIZE);
    if (!buf || publish(NULL,0)) return 1;
    for (;;) {
        int cl=accept(ls,NULL,NULL);
        if (cl<0) { if (errno==EINTR) continue; return 1; }
        struct ucred cred; socklen_t size=sizeof(cred);
        if (getsockopt(cl,SOL_SOCKET,SO_PEERCRED,&cred,&size) || cred.uid!=uid) { close(cl); continue; }
        struct timeval timeout={.tv_sec=45};
        setsockopt(cl,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        unsigned char header[4];
        uint32_t last_request=0;
        if (pread(ring,&last_request,4,OFF+0x80)!=4) last_request=0;
        for (;;) {
            struct pollfd pfd={.fd=cl,.events=POLLIN};
            int ready=poll(&pfd,1,1000);
            if (ready<0) { if (errno==EINTR) continue; break; }
            if (ready && (pfd.revents & (POLLERR|POLLHUP|POLLNVAL))) break;
            if (ready && (pfd.revents & POLLIN)) {
                if (read_full(cl,header,4)) break;
                uint32_t n=u32(header);
                if (n>MAX_SIZE || read_full(cl,buf,n) || !valid(buf,n) || publish(buf,n)) break;
            }
            uint32_t request=0;
            if (pread(ring,&request,4,OFF+0x80)==4 && request!=last_request) {
                if (send(cl,"S",1,MSG_NOSIGNAL)!=1) break;
                last_request=request;
            }
        }
        close(cl);
        if (publish(NULL,0)) return 1;
    }
}
