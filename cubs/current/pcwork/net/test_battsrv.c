#define main battsrv_program_main
#include "battsrv.c"
#undef main
#include <assert.h>
int main(void) {
    assert(usb_type_from_text("Unknown SDP DCP [CDP]\n")==3);
    assert(usb_type_from_text("[PD_PPS] PD\n")==8);
    assert(usb_type_from_text("SDP CDP PD\n")==0);
    assert(usb_type_from_text("[future-type]\n")==0);
    assert(usb_type_from_text("[PD\n")==0);
    puts("USB type parsing passed");
    return 0;
}
