/* Experimental one-request-at-a-time iOS-to-Android join transport.
 * Shared area: JBR2 at 0, response seq/status at 0x80/0x84,
 * claim seq at 0xc0, request seq/length at 0x100/0x104,
 * decision at 0x180 (seq=commit, seq|1=cancel), request bytes at 0x200.
 * A positive reply means Binder accepted a request, not that Wi-Fi associated.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OFF 0x2200000
#define MAGIC 0x3252424au
#define FRAME_MAX 109u
static void wipe(void *p,size_t n) { volatile unsigned char *q=p; while (n--) *q++=0; }
static int write_all(int fd,const unsigned char *p,size_t n) {
    while (n) { ssize_t w=write(fd,p,n); if (w<0 && errno==EINTR) continue; if (w<=0) return -1; p+=w; n-=(size_t)w; }
    return 0;
}
static int valid(const unsigned char *p,uint32_t n) {
    if (n<8 || memcmp(p,"JON1",4)) return 0;
    unsigned s=p[4], mode=p[5], b=p[6], k=p[7];
    if (!s || s>32 || (b!=0 && b!=6) || mode>3 || n!=8+s+b+k) return 0;
    if ((mode==0 && k!=0) || (mode==1 && (k<8 || k>63)) || (mode==2 && k!=32) ||
        (mode==3 && (k<1 || k>63))) return 0;
    if (mode==1 || mode==3) for (unsigned i=8+s+b;i<n;i++) if (p[i]<32 || p[i]>126) return 0;
    return 1;
}
/* Pipe reads can split a single helper line. Use one overall deadline and
 * accept only the exact success line, never a prefix or partial read. */
static int accepted_line(int fd) {
    char result[64] = {0}; size_t used=0;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC,&start)) return 0;
    int ok=0;
    while (used < sizeof(result)-1) {
        if (clock_gettime(CLOCK_MONOTONIC,&now)) break;
        int64_t elapsed=(now.tv_sec-start.tv_sec)*1000LL+(now.tv_nsec-start.tv_nsec)/1000000LL;
        if (elapsed>=15000) break;
        struct pollfd pfd={.fd=fd,.events=POLLIN};
        int ready=poll(&pfd,1,(int)(15000-elapsed));
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0) break;
        ssize_t got=read(fd,result+used,sizeof(result)-1-used);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) break;
        used+=(size_t)got;
        if (memchr(result,'\n',used)) {
            ok=used==sizeof("request-accepted\n")-1 &&
                memcmp(result,"request-accepted\n",used)==0;
            break;
        }
    }
    wipe(result,sizeof(result));
    return ok;
}
static int invoke(unsigned char *data,uint32_t n) {
    int in[2],out[2];
    if (pipe(in)) return 0;
    if (pipe(out)) { close(in[0]); close(in[1]); return 0; }
    pid_t child=fork();
    if (child<0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return 0; }
    if (child==0) {
        dup2(in[0],STDIN_FILENO); dup2(out[1],STDOUT_FILENO);
        int nullfd=open("/dev/null",O_WRONLY); if (nullfd>=0) dup2(nullfd,STDERR_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
#ifdef CUBS_JOIN_TEST
        const char *test_helper=getenv("CUBS_JOIN_TEST_HELPER");
        if (test_helper) { execl(test_helper,test_helper,(char *)NULL); _exit(127); }
#endif
        /* /data/local/ios is world-writable on this phone; never load root
         * code from there. The helper must be installed in a root-only path. */
        struct stat parent, dir, dex;
        if (lstat("/data/adb",&parent) || !S_ISDIR(parent.st_mode) || parent.st_uid!=0 || (parent.st_mode&022) ||
            lstat("/data/adb/cubs-wifi",&dir) || !S_ISDIR(dir.st_mode) || dir.st_uid!=0 || (dir.st_mode&077) ||
            lstat("/data/adb/cubs-wifi/cubs-wifi-join-helper.dex",&dex) || !S_ISREG(dex.st_mode) || dex.st_uid!=0 || (dex.st_mode&077)) _exit(126);
        setenv("CLASSPATH","/data/adb/cubs-wifi/cubs-wifi-join-helper.dex",1);
        execl("/system/bin/app_process","app_process","/system/bin","com.cubs.cam.PrivilegedWifiJoin",(char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    int ok=write_all(in[1],data,n)==0;
    close(in[1]); wipe(data,n);
    if (ok) ok=accepted_line(out[0]);
    close(out[0]);
    /* The Binder callback has already completed; do not let an app_process
     * thread pool keep credentials or this daemon alive indefinitely. */
    kill(child,SIGKILL);
    while (waitpid(child,NULL,0)<0 && errno==EINTR) { }
    return ok;
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    const char *path=getenv("CUBS_JOIN_RING");
    int fd=open(path?path:"/dev/cubs_ram",O_RDWR|O_NOFOLLOW); if (fd<0) return 1;
    struct stat st;
    if (fstat(fd,&st) || st.st_uid!=0 || (st.st_mode&022)) return 1;
    uint32_t zero=0,magic=MAGIC,last=0; unsigned char data[FRAME_MAX],blank[FRAME_MAX]={0};
    if (pwrite(fd,blank,sizeof(blank),OFF+0x200)!=(ssize_t)sizeof(blank) ||
        pwrite(fd,&zero,4,OFF+0x100)!=4 || pwrite(fd,&zero,4,OFF+0x104)!=4 ||
        pwrite(fd,&zero,4,OFF+0x80)!=4 || pwrite(fd,&zero,4,OFF+0x84)!=4 ||
        pwrite(fd,&zero,4,OFF+0xc0)!=4 || pwrite(fd,&zero,4,OFF+0x180)!=4 || pwrite(fd,&magic,4,OFF)!=4) return 1;
    for (;;) {
        uint32_t seq=0,len=0,check=0,status=2;
        if (pread(fd,&seq,4,OFF+0x100)!=4) return 1;
        if (!seq || (seq&1) || seq==last) { usleep(100000); continue; }
        if (pread(fd,&len,4,OFF+0x104)!=4) return 1;
        int copied=len<=FRAME_MAX && pread(fd,data,len,OFF+0x200)==(ssize_t)len;
        if (pread(fd,&check,4,OFF+0x100)!=4) return 1;
        if (check!=seq || (check&1)) { wipe(data,sizeof(data)); continue; }
        /* Clear even malformed frames before replying or invoking the helper. */
        if (pwrite(fd,blank,sizeof(blank),OFF+0x200)!=(ssize_t)sizeof(blank)) return 1;
        if (copied && valid(data,len)) {
            /* A copied request must never reach Binder without the sender's
             * explicit commit. A late reader observes cancellation instead. */
            if (pwrite(fd,&seq,4,OFF+0xc0)!=4) return 1;
            uint32_t decision=0;
            for (unsigned i=0;i<200;i++) {
                if (pread(fd,&decision,4,OFF+0x180)!=4) return 1;
                if (decision==seq || decision==(seq|1u)) break;
                usleep(100000);
            }
            if (decision==seq) status=invoke(data,len) ? 1 : 3;
        }
        wipe(data,sizeof(data));
        if (pwrite(fd,&status,4,OFF+0x84)!=4 || pwrite(fd,&seq,4,OFF+0x80)!=4) return 1;
        last=seq;
    }
}
