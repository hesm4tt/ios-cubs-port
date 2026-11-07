/* Offline validation only: no Wi-Fi service or network access. */
#define main wifijoinsrv_program_main
#include "wifijoinsrv.c"
#undef main
#include <assert.h>

int main(void) {
    unsigned char frame[8 + 1 + 63] = {'J','O','N','1',1,3,0,1,'x','a'};
    assert(valid(frame, 10));
    frame[7] = 0;
    assert(!valid(frame, 9));
    frame[7] = 1; frame[9] = 0x1f;
    assert(!valid(frame, 10));
    frame[9] = 0x7f;
    assert(!valid(frame, 10));
    memset(frame + 9, 'a', 63); frame[7] = 63;
    assert(valid(frame, sizeof(frame)));
    frame[7] = 64;
    assert(!valid(frame, sizeof(frame)));
    puts("SAE frame validation passed");
    int p[2]; assert(pipe(p)==0);
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        close(p[0]);
        assert(write(p[1],"request-",8)==8);
        usleep(50000);
        assert(write(p[1],"accepted\n",9)==9);
        close(p[1]); _exit(0);
    }
    close(p[1]); assert(accepted_line(p[0])); close(p[0]);
    assert(waitpid(child,NULL,0)==child);
    assert(pipe(p)==0);
    assert(write(p[1],"request-accepted",16)==16); close(p[1]);
    assert(!accepted_line(p[0])); close(p[0]);
    puts("split and incomplete acknowledgement checks passed");
    return 0;
}
