/* Android-only synthetic benchmark: compare GLES mipmaps with three linear blits. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#define W 1125
#define H 2436
#define N 24
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
int main(void) {
    EGLint n,major,minor; EGLConfig cfg;
    EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLint pa[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE}, xa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d,&major,&minor) || !eglChooseConfig(d,ca,&cfg,1,&n) || n<1) return 1;
    EGLSurface surface=eglCreatePbufferSurface(d,cfg,pa); EGLContext context=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa);
    if (!eglMakeCurrent(d,surface,surface,context)) return 2;
    GLuint tex[4],fb[4],mf; glGenTextures(4,tex); glGenFramebuffers(4,fb); glGenFramebuffers(1,&mf);
    for (int i=0;i<4;i++) {
        glBindTexture(GL_TEXTURE_2D,tex[i]); glTexStorage2D(GL_TEXTURE_2D,i==0?4:1,GL_RGBA8,W>>i,H>>i);
        glBindFramebuffer(GL_FRAMEBUFFER,fb[i]); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[i],0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 3;
    }
    glBindFramebuffer(GL_FRAMEBUFFER,mf); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[0],3);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 4;
    size_t bytes=(size_t)(W>>3)*(H>>3)*4; uint8_t *a=malloc(bytes),*b=malloc(bytes);
    if (!a || !b) return 5;
    double mip=0,blit=0; unsigned maxdiff=0; size_t different=0;
    for (int frame=0;frame<N;frame++) {
        glBindFramebuffer(GL_FRAMEBUFFER,fb[0]); glViewport(0,0,W,H);
        glClearColor((frame%3)*0.3f,(frame%5)*0.2f,0.25f,1); glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor((frame*61)%W,(frame*97)%H,117,231);
        glClearColor(1,0.2f,0.1f,1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        double t=ms(); glBindTexture(GL_TEXTURE_2D,tex[0]); glGenerateMipmap(GL_TEXTURE_2D);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,mf); glReadPixels(0,0,W>>3,H>>3,GL_RGBA,GL_UNSIGNED_BYTE,a); mip+=ms()-t;
        t=ms();
        for(int i=1;i<4;i++) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,fb[i-1]); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fb[i]);
            glBlitFramebuffer(0,0,W>>(i-1),H>>(i-1),0,0,W>>i,H>>i,GL_COLOR_BUFFER_BIT,GL_LINEAR);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fb[3]); glReadPixels(0,0,W>>3,H>>3,GL_RGBA,GL_UNSIGNED_BYTE,b); blit+=ms()-t;
        for(size_t i=0;i<bytes;i++) { unsigned diff=a[i]>b[i]?a[i]-b[i]:b[i]-a[i]; if(diff) different++; if(diff>maxdiff)maxdiff=diff; }
        if (glGetError()!=GL_NO_ERROR) return 6;
    }
    printf("mipmap %.2f ms, three-blit %.2f ms, differing bytes %zu, max difference %u\n",mip/N,blit/N,different,maxdiff);
    free(a); free(b); return 0;
}
