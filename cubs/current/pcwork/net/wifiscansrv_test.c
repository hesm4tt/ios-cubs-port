/* Android-only synthetic transport test; no real Wi-Fi data. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    uid_t uid=(uid_t)atoi(argv[1]);
    if (setuid(uid)) return 3;
    int s=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un a={.sun_family=AF_UNIX};
    const char *name="cubs_wifi_scan";
    memcpy(a.sun_path+1,name,strlen(name));
    if (connect(s,(struct sockaddr *)&a,offsetof(struct sockaddr_un,sun_path)+1+strlen(name))) return 4;
    /* Valid empty cached snapshot; all fields little endian. */
    unsigned char packet[20]={16,0,0,0,'S','C','N','1',1,0,0,0,0,0,0,0,0,0,0,0};
    if (argc == 3 && !strcmp(argv[2], "malformed")) packet[4] = 'X';
    if (write(s,packet,sizeof(packet)) != sizeof(packet)) return 5;
    if (argc == 3 && !strcmp(argv[2], "request")) {
        struct timeval t={.tv_sec=5}; char command=0;
        setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));
        if (read(s,&command,1)!=1 || command!='S') return 6;
        puts("request-received"); close(s); return 0;
    }
    sleep(2);
    close(s);
    return 0;
}
