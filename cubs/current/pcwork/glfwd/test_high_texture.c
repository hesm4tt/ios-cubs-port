/* Direct Android EGL exercise of the server's high-name fallback. */
#define main glsrv_program_main
#include "glsrv.c"
#undef main
#include <assert.h>

int main(void) {
    assert(make_context());
    GLuint ids[4200], fb;
    glGenTextures(4200, ids);
    GLuint tex=0;
    for (unsigned i=0;i<4200;i++) if (ids[i]>=TMAX) { tex=ids[i]; break; }
    if (!tex) { puts("no high texture name allocated"); return 2; }
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fb); assert(fb<TMAX);
    glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glViewport(0,0,32,32); glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    cur_read_fbo=fb; fbo_color[fb]=tex;
    sshadow_size[0]=32*32*4; sshadow[0]=calloc(1,sshadow_size[0]); assert(sshadow[0]);
    uint32_t a[]={0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,128,0,1,0};
    reply_len=0; do_op(OP_READ_PIXELS_DELTA,a,10,NULL,0);
    uint32_t mode,nb,row,rows;
    memcpy(&mode,reply,4); memcpy(&nb,reply+4,4);
    memcpy(&row,reply+12,4); memcpy(&rows,reply+16,4);
    assert(mode==1 && nb==1 && row==0 && rows==32 && reply_len==20+32*128);
    assert(reply[20]==255 && reply[21]==0 && reply[22]==0 && reply[23]==255);
    assert(glGetError()==GL_NO_ERROR);
    puts("high texture fallback returned full red frame");
    return 0;
}
