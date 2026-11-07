#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#define W 1125
#define H 2436
#define N 80
static double ms(void){struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6;}
int main(void){EGLint maj,min,n; EGLConfig cfg; EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE}; EGLint pa[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE}, xa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); if(!eglInitialize(d,&maj,&min)||!eglChooseConfig(d,ca,&cfg,1,&n)||n<1) return 1; EGLSurface s=eglCreatePbufferSurface(d,cfg,pa); EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); if(!eglMakeCurrent(d,s,s,c))return 2; printf("renderer %s\n",glGetString(GL_RENDERER)); GLuint tex,fb,pbo[3]; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,W,H); glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0); if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return 3; const size_t sz=(size_t)W*H*4; uint8_t *out=malloc(sz); if(!out)return 4; glViewport(0,0,W,H); double t=ms(); for(int i=0;i<N;i++){glClearColor((i%2)?1:0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,out);if(out[0]!=((i%2)?255:0)){puts("sync wrong pixels");return 5;}} printf("sync %.2f ms/frame\n",(ms()-t)/N); glGenBuffers(3,pbo); for(int i=0;i<3;i++){glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[i]);glBufferData(GL_PIXEL_PACK_BUFFER,sz,NULL,GL_STREAM_READ);} glBindBuffer(GL_PIXEL_PACK_BUFFER,0); t=ms(); double submit=0,map=0; for(int i=0;i<N+2;i++){if(i<N){glClearColor((i%2)?1:0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[i%3]);double a=ms();glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,0);glFlush();submit+=ms()-a;} if(i>=2){int k=i-2;glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[k%3]);double a=ms();uint8_t *p=glMapBufferRange(GL_PIXEL_PACK_BUFFER,0,sz,GL_MAP_READ_BIT);if(!p||p[0]!=((k%2)?255:0)){puts("pbo wrong pixels");return 6;}memcpy(out,p,sz);glUnmapBuffer(GL_PIXEL_PACK_BUFFER);map+=ms()-a;}} printf("pbo total %.2f submit %.2f map-copy %.2f ms/frame\n",(ms()-t)/N,submit/N,map/N); printf("gl error %#x\n",glGetError());return 0;}

