/*
 * glsrv: the Android side of the OpenGL ES forwarding (gl_fwd.h). Runs as root from /data/local/tmp next to mtlsrv: opens the
 * shared ring (RING_DEV, RING_BASE like mtlsrv), makes one EGL context of OpenGL ES 3 on the phone's GPU (a 16x16 pbuffer
 * to hold it; CoreAnimation draws into framebuffer objects) and executes the batches iOS's window server writes.
 */
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "gl_fwd.h"

static int ring_fd;
static long ring_base;

static void
rd(size_t off, void *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pread(ring_fd, (char *)p + done, n - done, ring_base + off + done);

		if (r <= 0) {
			perror("glsrv: ring read");
			exit(1);
		}
		done += r;
	}
}

static void
wr(size_t off, const void *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pwrite(ring_fd, (const char *)p + done, n - done, ring_base + off + done);

		if (r <= 0) {
			perror("glsrv: ring write");
			exit(1);
		}
		done += r;
	}
}

static uint32_t
word(int i)
{
	uint32_t v = 0;

	rd(GL_OFF + i * 4, &v, 4);
	return v;
}

static void
setword(int i, uint32_t v)
{
	wr(GL_OFF + i * 4, &v, 4);
}

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface surf;
static GLuint vbo, ibo;

static int
make_context(void)
{
	EGLint major, minor, n = 0;
	EGLConfig cfg;
	const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	const EGLint pattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };

	if (ctx != EGL_NO_CONTEXT) {
		return 1;
	}
	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, &major, &minor)) {
		printf("glsrv: eglInitialize failed %x\n", eglGetError());
		return 0;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) {
		printf("glsrv: no config (%x)\n", eglGetError());
		return 0;
	}
	surf = eglCreatePbufferSurface(dpy, cfg, pattr);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		printf("glsrv: context failed (%x)\n", eglGetError());
		ctx = EGL_NO_CONTEXT;
		return 0;
	}
	glGenBuffers(1, &vbo);
	glGenBuffers(1, &ibo);
	printf("glsrv: EGL %d.%d, %s / %s / %s\n", major, minor, glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
	fflush(stdout);
	return 1;
}

static GLuint ucache_tex[8];
static uint32_t ucw[8], uch[8], ufmt[8], utype[8];
static uint8_t *sshadow[8], *scur;
static size_t sshadow_size[8], scur_size;
static double now(void);
static uint32_t last_op;
static double op_time[128];
static unsigned long op_count[128];
static uint8_t batch[GL_PAYLOAD_MAX + 16];
static uint8_t reply[GL_PAYLOAD_MAX + 16];
static uint32_t reply_len, result;

static float
fbits(uint32_t v)
{
	float f;

	memcpy(&f, &v, 4);
	return f;
}

static void
put_reply(const void *p, size_t n)
{
	if (reply_len + n > GL_PAYLOAD_MAX) {
		n = GL_PAYLOAD_MAX - reply_len;
	}
	memcpy(reply + reply_len, p, n);
	reply_len += n;
}

static int
ints_for_pname(GLenum p)
{
	switch (p) {
	case GL_VIEWPORT:
	case GL_SCISSOR_BOX:
	case GL_COLOR_CLEAR_VALUE:
	case GL_COLOR_WRITEMASK:
	case GL_BLEND_COLOR:
		return 4;
	case GL_ALIASED_LINE_WIDTH_RANGE:
	case GL_ALIASED_POINT_SIZE_RANGE:
	case GL_DEPTH_RANGE:
	case GL_MAX_VIEWPORT_DIMS:
		return 2;
	case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
	default:
		return 1;
	}
}

/* the arrays of a draw: uploaded into one buffer, the pointers made to point into it */
static const uint32_t *
setup_attribs(const uint32_t *d, uint32_t nattr, const uint8_t *blob, size_t *used)
{
	uint32_t enabled = 0;
	size_t total = 0;
	const uint8_t *b = blob;

	for (uint32_t i = 0; i < nattr; i++) {
		total += d[i * 6 + 5];
	}
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, total ? total : 4, NULL, GL_STREAM_DRAW);
	size_t off = 0;

	for (uint32_t i = 0; i < nattr; i++) {
		const uint32_t *a = d + i * 6;

		if (a[5]) {
			glBufferSubData(GL_ARRAY_BUFFER, off, a[5], b);
		}
		glEnableVertexAttribArray(a[0]);
		glVertexAttribPointer(a[0], a[1], a[2], a[3] != 0, a[4], (const void *)off);
		enabled |= 1u << a[0];
		off += a[5];
		b += (a[5] + 3) & ~3u;
	}
	for (GLuint i = 0; i < 16; i++) {
		if (!(enabled & (1u << i))) {
			glDisableVertexAttribArray(i);
		}
	}
	*used = (size_t)(b - blob);
	return d + nattr * 6;
}

/* ---- dirty rows: which texture a framebuffer draws into, and which rows of it have changed since it was read back */
#define TMAX 4096
#define DFULL_HI (1 << 30)
static GLuint cur_draw_fbo, cur_read_fbo, fbo_color[TMAX], bound2d[32];
static int act_unit, scissor_on, sc[4], dinit;
static int dy0[TMAX], dy1[TMAX], dx0[TMAX], dx1[TMAX];
static unsigned sfull_count[8];

static void
dmarkx(GLuint t, int x0, int x1, int y0, int y1)
{
	if (t && t < TMAX) {
		if (x0 < dx0[t]) {
			dx0[t] = x0;
		}
		if (x1 > dx1[t]) {
			dx1[t] = x1;
		}
		if (y0 < dy0[t]) {
			dy0[t] = y0;
		}
		if (y1 > dy1[t]) {
			dy1[t] = y1;
		}
	}
}

static void
dmark(GLuint t, int y0, int y1)
{
	dmarkx(t, 0, DFULL_HI, y0, y1);
}

static void
dfull(GLuint t)
{
	dmark(t, 0, DFULL_HI);
}

static void
track(uint32_t op, const uint32_t *a)
{
	GLuint t;

	if (!dinit) {
		for (int i = 0; i < TMAX; i++) {
			dy0[i] = 0;
			dy1[i] = DFULL_HI;
			dx0[i] = 0;
			dx1[i] = DFULL_HI;
		}
		dinit = 1;
	}
	switch (op) {
	case OP_ACTIVE_TEXTURE: act_unit = (int)(a[0] - 0x84C0) & 31; break;
	case OP_BIND_TEXTURE:
		if (a[0] == 0x0DE1) {
			bound2d[act_unit] = a[1];
		}
		break;
	case OP_BIND_FRAMEBUFFER:
		if (a[0] == 0x8D40) {
			cur_draw_fbo = cur_read_fbo = a[1];
		} else if (a[0] == 0x8CA9) {
			cur_draw_fbo = a[1];
		} else if (a[0] == 0x8CA8) {
			cur_read_fbo = a[1];
		}
		break;
	case OP_DELETE_FRAMEBUFFERS:
		for (uint32_t i = 0; i < a[0]; i++) {
			if (a[1 + i] < TMAX) {
				fbo_color[a[1 + i]] = 0;
			}
		}
		break;
	case OP_FRAMEBUFFER_TEX2D:
		if (a[1] == 0x8CE0) {
			GLuint f = a[0] == 0x8CA8 ? cur_read_fbo : cur_draw_fbo;

			if (f < TMAX) {
				fbo_color[f] = a[3];
			}
		}
		break;
	case OP_ENABLE:
		if (a[0] == 0x0C11) {
			scissor_on = 1;
		}
		break;
	case OP_DISABLE:
		if (a[0] == 0x0C11) {
			scissor_on = 0;
		}
		break;
	case OP_SCISSOR:
		memcpy(sc, a, 16);
		break;
	case OP_CLEAR:
	case OP_DRAW_ARRAYS:
	case OP_DRAW_ELEMENTS:
		t = cur_draw_fbo < TMAX ? fbo_color[cur_draw_fbo] : 0;
		if (t) {
			if (scissor_on) {
				dmarkx(t, sc[0], sc[0] + sc[2], sc[1], sc[1] + sc[3]);
				op_count[sc[3] >= 2000 ? 109 : 110]++;
				op_time[111] += (double)sc[2] * sc[3] / 1e6;
				op_count[111]++;
			} else {
				dfull(t);
				op_count[105]++;
			}
		}
		break;
	case OP_BLIT_FRAMEBUFFER:
		op_count[106]++;
		dfull(cur_draw_fbo < TMAX ? fbo_color[cur_draw_fbo] : 0);
		break;
	case OP_COPY_TEX_SUB_IMAGE_2D:
	case OP_TEX_IMAGE_2D:
	case OP_TEX_SUB_IMAGE_2D:
	case OP_COMPRESSED_TEX_IMAGE_2D:
	case OP_GEN_MIPMAP:
	case OP_TEX_IMAGE_FROM_CACHE:
		op_count[107]++;
		dfull(bound2d[act_unit]);
		break;
	case OP_DELETE_TEXTURES:
		for (uint32_t i = 0; i < a[0]; i++) {
			dfull(a[1 + i]);
		}
		break;
	}
}

/* ---- a small mipmap level of a texture, to tell whether it has changed */
static GLuint mip_fbo;
static uint8_t *smip[8], *mipscratch;
static size_t smip_size[8], mipscratch_size;

static int
mip_read(GLuint rt, uint32_t w, uint32_t h, size_t *outsize)
{
	GLint prev_tex = 0, prev_read = 0, tw = 0, th = 0, prl = 0, pal = 4;
	const int level = 3;
	uint32_t mw = w >> level, mh = h >> level;

	if (mw == 0) {
		mw = 1;
	}
	if (mh == 0) {
		mh = 1;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
	glBindTexture(GL_TEXTURE_2D, rt);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
	if (tw != (GLint)w || th != (GLint)h) {
		glBindTexture(GL_TEXTURE_2D, prev_tex);
		return 0;
	}
	glGenerateMipmap(GL_TEXTURE_2D);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	if (!mip_fbo) {
		glGenFramebuffers(1, &mip_fbo);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, mip_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, level);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &prl);
	glGetIntegerv(GL_PACK_ALIGNMENT, &pal);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (mipscratch_size < (size_t)mw * mh * 4) {
		mipscratch = realloc(mipscratch, (size_t)mw * mh * 4);
		mipscratch_size = (size_t)mw * mh * 4;
	}
	glReadPixels(0, 0, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, mipscratch);
	glPixelStorei(GL_PACK_ROW_LENGTH, prl);
	glPixelStorei(GL_PACK_ALIGNMENT, pal);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
	glBindTexture(GL_TEXTURE_2D, prev_tex);
	*outsize = (size_t)mw * mh * 4;
	return 1;
}

/* ---- the changed bands of a readback, paged over several replies */
static uint32_t pend_row0[8][260], pend_rows[8][260], pend_n[8], pend_pos[8];

/* Compositor-only pipeline. Ordinary GL readbacks remain synchronous. */
struct present_pipe {
	GLuint pbo[2], texture;
	size_t size;
	uint32_t w, h, format, type, stride;
	int pending;
};
static struct present_pipe present_pipe[8];

static int
present_read(uint32_t key, GLuint texture, const uint32_t *a, size_t size)
{
	struct present_pipe *p = &present_pipe[key];
	GLint previous = 0;
	int old, next;

	if (!texture || a[0] || a[1] || !size) return 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
	if (!p->pbo[0]) glGenBuffers(2, p->pbo);
	if (!p->pbo[0] || !p->pbo[1]) return 0;
	if (p->texture != texture || p->size != size || p->w != a[2] || p->h != a[3] ||
	    p->format != a[4] || p->type != a[5] || p->stride != a[6]) {
		p->texture = texture; p->size = size; p->w = a[2]; p->h = a[3];
		p->format = a[4]; p->type = a[5]; p->stride = a[6]; p->pending = 0;
		for (int i = 0; i < 2; i++) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[i]);
			glBufferData(GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
		}
	}
	old = p->pending - 1;
	next = old == 0 ? 1 : 0;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[next]);
	glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], 0);
	glFlush();
	if (old >= 0) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[old]);
		void *pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);
		if (!pixels) {
			p->pending = 0;
			glBindBuffer(GL_PIXEL_PACK_BUFFER, previous);
			return 0;
		}
		memcpy(scur, pixels, size);
		if (!glUnmapBuffer(GL_PIXEL_PACK_BUFFER)) {
			p->pending = 0;
			glBindBuffer(GL_PIXEL_PACK_BUFFER, previous);
			return 0;
		}
	} else {
		/* Prime the pipeline without presenting an uninitialised frame. */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], scur);
	}
	p->pending = next + 1;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, previous);
	return 1;
}

static uint32_t
emit_pending(uint32_t key, size_t stride, uint32_t *more)
{
	uint32_t nb = 0;

	while (pend_pos[key] < pend_n[key]) {
		uint32_t row0 = pend_row0[key][pend_pos[key]], rows = pend_rows[key][pend_pos[key]];
		uint32_t hdr[2] = { row0, rows };

		if (reply_len + 8 + (size_t)rows * stride > GL_PAYLOAD_MAX) {
			break;
		}
		memcpy(reply + reply_len, hdr, 8);
		memcpy(reply + reply_len + 8, scur + (size_t)row0 * stride, (size_t)rows * stride);
		reply_len += 8 + (size_t)rows * stride;
		pend_pos[key]++;
		nb++;
	}
	*more = pend_pos[key] < pend_n[key];
	return nb;
}

static void
do_op(uint32_t op, const uint32_t *a, uint32_t nargs, const uint8_t *blob, uint32_t bloblen)
{
	(void)nargs;
	track(op, a);
	switch (op) {
	case OP_ACTIVE_TEXTURE: glActiveTexture(a[0]); break;
	case OP_BIND_TEXTURE: glBindTexture(a[0], a[1]); break;
	case OP_BLEND_EQ: glBlendEquation(a[0]); break;
	case OP_BLEND_FUNC: glBlendFunc(a[0], a[1]); break;
	case OP_BLEND_FUNC_SEP: glBlendFuncSeparate(a[0], a[1], a[2], a[3]); break;
	case OP_CLEAR: glClear(a[0]); break;
	case OP_CLEAR_COLOR: glClearColor(fbits(a[0]), fbits(a[1]), fbits(a[2]), fbits(a[3])); break;
	case OP_CLEAR_STENCIL: glClearStencil(a[0]); break;
	case OP_COPY_TEX_SUB_IMAGE_2D: glCopyTexSubImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
	case OP_DELETE_TEXTURES: glDeleteTextures(a[0], a + 1); break;
	case OP_DISABLE: glDisable(a[0]); break;
	case OP_ENABLE: glEnable(a[0]); break;
	case OP_SCISSOR: glScissor(a[0], a[1], a[2], a[3]); break;
	case OP_VIEWPORT: glViewport(a[0], a[1], a[2], a[3]); break;
	case OP_PIXEL_STORE: glPixelStorei(a[0], a[1]); break;
	case OP_TEX_PARAM_I: glTexParameteri(a[0], a[1], a[2]); break;
	case OP_TEX_PARAM_F: glTexParameterf(a[0], a[1], fbits(a[2])); break;
	case OP_TEX_IMAGE_2D: glTexImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8] ? blob : NULL); break;
	case OP_TEX_SUB_IMAGE_2D: glTexSubImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], blob); break;
	case OP_COMPRESSED_TEX_IMAGE_2D: glCompressedTexImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], blob); break;
	case OP_GEN_MIPMAP: glGenerateMipmap(a[0]); break;
	case OP_SHADER_SOURCE: {
		const GLchar *s = (const GLchar *)blob;
		GLint len = bloblen;

		/* the blob is padded to 4 and carries its own length in the first argument word 1 */
		len = a[1];
		glShaderSource(a[0], 1, &s, &len);
		break;
	}
	case OP_COMPILE_SHADER: glCompileShader(a[0]); break;
	case OP_ATTACH_SHADER: glAttachShader(a[0], a[1]); break;
	case OP_LINK_PROGRAM: glLinkProgram(a[0]); break;
	case OP_USE_PROGRAM: glUseProgram(a[0]); break;
	case OP_DELETE_PROGRAM: glDeleteProgram(a[0]); break;
	case OP_UNIFORM1F: glUniform1f(a[0], fbits(a[1])); break;
	case OP_UNIFORM2F: glUniform2f(a[0], fbits(a[1]), fbits(a[2])); break;
	case OP_UNIFORM4F: glUniform4f(a[0], fbits(a[1]), fbits(a[2]), fbits(a[3]), fbits(a[4])); break;
	case OP_UNIFORM1I: glUniform1i(a[0], a[1]); break;
	case OP_UNIFORM_VEC: {
		const GLfloat *f = (const GLfloat *)blob;

		if (a[2] == 1) {
			glUniform1fv(a[0], a[1], f);
		} else if (a[2] == 2) {
			glUniform2fv(a[0], a[1], f);
		} else if (a[2] == 3) {
			glUniform3fv(a[0], a[1], f);
		} else {
			glUniform4fv(a[0], a[1], f);
		}
		break;
	}
	case OP_UNIFORM_MAT: {
		const GLfloat *f = (const GLfloat *)blob;

		if (a[2] == 3) {
			glUniformMatrix3fv(a[0], a[1], a[3] != 0, f);
		} else {
			glUniformMatrix4fv(a[0], a[1], a[3] != 0, f);
		}
		break;
	}
	case OP_BIND_ATTRIB_LOC: {
		char name[256];
		uint32_t n = a[2] < 255 ? a[2] : 255;

		memcpy(name, blob, n);
		name[n] = 0;
		glBindAttribLocation(a[0], a[1], name);
		break;
	}
	case OP_BIND_FRAMEBUFFER: glBindFramebuffer(a[0], a[1]); break;
	case OP_DELETE_FRAMEBUFFERS: glDeleteFramebuffers(a[0], a + 1); break;
	case OP_FRAMEBUFFER_TEX2D: glFramebufferTexture2D(a[0], a[1], a[2], a[3], a[4]); break;
	case OP_DRAW_ARRAYS: {
		size_t used;

		setup_attribs(a + 4, a[3], blob, &used);
		glDrawArrays(a[0], 0, a[2]);
		break;
	}
	case OP_DRAW_ELEMENTS: {
		size_t used;
		const uint32_t nattr = a[3], idxlen = a[4];
		const uint8_t *arrays = blob + ((idxlen + 3) & ~3u);

		setup_attribs(a + 5, nattr, arrays, &used);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, idxlen ? idxlen : 4, blob, GL_STREAM_DRAW);
		glDrawElements(a[0], a[1], a[2], (const void *)0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		break;
	}
	case OP_FLUSH: glFlush(); break;
	case OP_CACHE_DEFINE: {
		uint32_t key = a[0] & 7;
		GLint prev = 0;

		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
		if (!ucache_tex[key]) {
			glGenTextures(1, &ucache_tex[key]);
		}
		ucw[key] = a[1];
		uch[key] = a[2];
		ufmt[key] = a[4];
		utype[key] = a[5];
		glBindTexture(GL_TEXTURE_2D, ucache_tex[key]);
		glTexImage2D(GL_TEXTURE_2D, 0, a[3], a[1], a[2], 0, a[4], a[5], NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, prev);
		break;
	}
	case OP_CACHE_PATCH: {
		uint32_t key = a[0] & 7;
		GLint prev = 0;

		if (ucache_tex[key]) {
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
			glBindTexture(GL_TEXTURE_2D, ucache_tex[key]);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, a[1], ucw[key], a[2], ufmt[key], utype[key], blob);
			glBindTexture(GL_TEXTURE_2D, prev);
		}
		break;
	}
	case OP_TEX_IMAGE_FROM_CACHE: {
		uint32_t key = a[0] & 7;
		GLint cur = 0;

		glGetIntegerv(GL_TEXTURE_BINDING_2D, &cur);
		glTexImage2D(a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], NULL);
		if (ucache_tex[key] && cur) {
			glCopyImageSubData(ucache_tex[key], GL_TEXTURE_2D, 0, 0, 0, 0, cur, GL_TEXTURE_2D, 0, 0, 0, 0, a[4], a[5], 1);
		}
		break;
	}
	case OP_DELETE_SHADER: glDeleteShader(a[0]); break;
	case OP_BLIT_FRAMEBUFFER: glBlitFramebuffer(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;

	case OP_CREATE_CTX: result = make_context(); break;
	case OP_FINISH: glFinish(); break;
	case OP_GEN_TEXTURES: {
		GLuint ids[64];
		uint32_t n = a[0] < 64 ? a[0] : 64;

		glGenTextures(n, ids);
		put_reply(ids, n * 4);
		break;
	}
	case OP_GEN_FRAMEBUFFERS: {
		GLuint ids[64];
		uint32_t n = a[0] < 64 ? a[0] : 64;

		glGenFramebuffers(n, ids);
		put_reply(ids, n * 4);
		break;
	}
	case OP_CREATE_SHADER: result = glCreateShader(a[0]); break;
	case OP_CREATE_PROGRAM: result = glCreateProgram(); break;
	case OP_GET_STRING: {
		const char *s = (const char *)glGetString(a[0]);

		put_reply(s ? s : "", s ? strlen(s) + 1 : 1);
		break;
	}
	case OP_GET_INTEGERV: {
		GLint v[64] = { 0 };
		int n = ints_for_pname(a[0]);

		glGetIntegerv(a[0], v);
		put_reply(v, n * 4);
		break;
	}
	case OP_GET_FLOATV: {
		GLfloat v[64] = { 0 };
		int n = ints_for_pname(a[0]);

		glGetFloatv(a[0], v);
		put_reply(v, n * 4);
		break;
	}
	case OP_GET_UNIFORM_LOCATION: {
		char name[256];
		uint32_t n = a[1] < 255 ? a[1] : 255;

		memcpy(name, blob, n);
		name[n] = 0;
		result = (uint32_t)glGetUniformLocation(a[0], name);
		break;
	}
	case OP_GET_PROGRAMIV: {
		GLint v = 0;

		glGetProgramiv(a[0], a[1], &v);
		result = v;
		break;
	}
	case OP_GET_SHADERIV: {
		GLint v = 0;

		glGetShaderiv(a[0], a[1], &v);
		result = v;
		break;
	}
	case OP_GET_SHADER_INFO_LOG:
	case OP_GET_PROGRAM_INFO_LOG: {
		char log[4096];
		GLsizei n = 0;

		log[0] = 0;
		if (op == OP_GET_SHADER_INFO_LOG) {
			glGetShaderInfoLog(a[0], sizeof log, &n, log);
		} else {
			glGetProgramInfoLog(a[0], sizeof log, &n, log);
		}
		put_reply(log, n + 1);
		break;
	}
	case OP_READ_PIXELS: {
		size_t size = a[6];

		if (size > GL_PAYLOAD_MAX) {
			size = GL_PAYLOAD_MAX;
		}
		glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], reply);
		reply_len = size;
		break;
	}
	case OP_GET_TEXLEVELPARAM: {
		GLint v = 0;

		glGetTexLevelParameteriv(a[0], a[1], a[2], &v);
		result = v;
		break;
	}
	case OP_GET_TEXPARAM: {
		GLint v = 0;

		glGetTexParameteriv(a[0], a[1], &v);
		result = v;
		break;
	}
	case OP_GET_FB_ATTACH_PARAM: {
		GLint v = 0;

		glGetFramebufferAttachmentParameteriv(a[0], a[1], a[2], &v);
		result = v;
		break;
	}
	case OP_GET_ACTIVE_UNIFORM: {
		GLint size = 0;
		GLenum type = 0;
		GLsizei n = 0;
		char name[256];

		name[0] = 0;
		glGetActiveUniform(a[0], a[1], sizeof name, &n, &size, &type, name);
		put_reply(&size, 4);
		put_reply(&type, 4);
		put_reply(name, n + 1);
		break;
	}
	case OP_GET_ATTACHED_SHADERS: {
		GLuint ids[16];
		GLsizei n = 0;

		glGetAttachedShaders(a[0], a[1] < 16 ? a[1] : 16, &n, ids);
		put_reply(&n, 4);
		put_reply(ids, n * 4);
		break;
	}
	case OP_IS_TEXTURE: result = glIsTexture(a[0]); break;
	case OP_IS_PROGRAM: result = glIsProgram(a[0]); break;
	case OP_GET_UNIFORMFV: {
		GLfloat v[16] = { 0 };

		glGetUniformfv(a[0], a[1], v);
		put_reply(v, sizeof v);
		break;
	}
	case OP_GET_UNIFORMIV: {
		GLint v[16] = { 0 };

		glGetUniformiv(a[0], a[1], v);
		put_reply(v, sizeof v);
		break;
	}
	case OP_IS_ENABLED: result = glIsEnabled(a[0]); break;
	case OP_READ_PIXELS_PRESENT:
	case OP_READ_PIXELS_DELTA: {
		size_t stride = a[6], rows_total = a[3], size = stride * rows_total;
		uint32_t key = a[7] & 7, valid = a[8], mode = 0, nb = 0, more = 0;
		GLuint rt = cur_read_fbo < TMAX ? fbo_color[cur_read_fbo] : 0;

		if (a[9]) {
			uint32_t m1 = 1;

			reply_len = 12;
			nb = emit_pending(key, stride, &more);
			memcpy(reply, &m1, 4);
			memcpy(reply + 4, &nb, 4);
			memcpy(reply + 8, &more, 4);
			break;
		}
		size_t r0 = 0, r1 = rows_total;         /* the rows (from the first asked for) that are read and compared */
		int partial = 0;

		if (size > scur_size) {
			scur = realloc(scur, size);
			scur_size = size;
		}
		size_t msz = 0;
		int piped = op == OP_READ_PIXELS_PRESENT && present_read(key, rt, a, size);
		int have_mip = !piped && rt && a[0] == 0 && a[1] == 0 && mip_read(rt, a[2], a[3], &msz);

		if (!piped && valid && sshadow[key] && sshadow_size[key] == size && rt && (++sfull_count[key] % 64) != 0) {
			int d0 = dy0[rt] > (int)a[1] ? dy0[rt] : (int)a[1];
			int d1 = dy1[rt] < (int)(a[1] + a[3]) ? dy1[rt] : (int)(a[1] + a[3]);

			partial = 1;
			if (d1 <= d0) {
				r0 = r1 = 0;
			} else {
				r0 = (size_t)d0 - a[1];
				r1 = (size_t)d1 - a[1];
			}			if (have_mip && smip[key] && smip_size[key] == msz) {
				uint32_t mw = a[2] >> 3 ? a[2] >> 3 : 1, mh = a[3] >> 3 ? a[3] >> 3 : 1;
				size_t rowb = (size_t)mw * 4;
				long first = -1, last = -1;

				for (uint32_t my = 0; my < mh; my++) {
					if (memcmp(smip[key] + my * rowb, mipscratch + my * rowb, rowb) != 0) {
						if (first < 0) {
							first = my;
						}
						last = my;
					}
				}
				if (first < 0) {
					r0 = r1 = 0;                    /* unchanged: nothing is read, the client's copy stands */
					op_count[113]++;
				} else {
					long y0 = first * 8 - 8, y1 = (last + 1) * 8 + 8;

					if (y0 < 0) {
						y0 = 0;
					}
					if (y1 > (long)a[3]) {
						y1 = (long)a[3];
					}
					r0 = (size_t)y0;
					r1 = (size_t)y1;
					op_count[114]++;
				}
			}
		}
		if (!piped && r1 > r0) {
			glReadPixels(a[0], (GLint)(a[1] + r0), a[2], (GLsizei)(r1 - r0), a[4], a[5], scur + r0 * stride);
		}
		op_time[103] += (double)(r1 - r0) / 1000.0;     /* (rows read, in thousands, over op_count[103] readbacks) */
		op_count[103]++;
		if (rt) {
			int bw = dx1[rt] > 1125 ? 1125 : dx1[rt], bh = (int)(r1 - r0);

			if (bw > dx0[rt] && bh > 0) {
				op_time[112] += (double)(bw - dx0[rt]) * bh / 1e6;
			}
			op_count[112]++;
			dy0[rt] = DFULL_HI;
			dy1[rt] = 0;
			dx0[rt] = DFULL_HI;
			dx1[rt] = 0;
		}
		if (have_mip) {
			if (smip_size[key] != msz) {
				smip[key] = realloc(smip[key], msz);
				smip_size[key] = msz;
			}
			memcpy(smip[key], mipscratch, msz);
		}
		reply_len = 12;
		pend_n[key] = 0;
		pend_pos[key] = 0;
		if (valid && sshadow[key] && sshadow_size[key] == size) {
			int overflow = 0;

			for (size_t row0 = r0; row0 < r1; row0 += 32) {
				size_t rows = r1 - row0 < 32 ? r1 - row0 : 32;

				if (memcmp(scur + row0 * stride, sshadow[key] + row0 * stride, rows * stride) != 0) {
					if (pend_n[key] >= 260) {
						overflow = 1;
						break;
					}
					pend_row0[key][pend_n[key]] = (uint32_t)row0;
					pend_rows[key][pend_n[key]] = (uint32_t)rows;
					pend_n[key]++;
				}
			}
			if (!overflow) {
				mode = 1;
				nb = emit_pending(key, stride, &more);
			}
		}
		if (mode == 0) {
			reply_len = 12;
			nb = 0;
			more = 0;
		}
		memcpy(reply, &mode, 4);
		memcpy(reply + 4, &nb, 4);
		memcpy(reply + 8, &more, 4);
		if (sshadow_size[key] != size) {
			sshadow[key] = realloc(sshadow[key], size);
			sshadow_size[key] = size;
		}
		if (partial) {
			if (r1 > r0) {
				memcpy(sshadow[key] + r0 * stride, scur + r0 * stride, (r1 - r0) * stride);
			}
		} else {
			memcpy(sshadow[key], scur, size);
		}
		break;
	}
	default:
		printf("glsrv: unknown op %u\n", op);
		break;
	}
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int
main(void)
{
	const char *path = getenv("RING_DEV") ? getenv("RING_DEV") : "/dev/cubs_ram";
	long base = getenv("RING_BASE") ? strtol(getenv("RING_BASE"), NULL, 16) : 0;
	uint32_t last = 0;
	unsigned long calls = 0, ops = 0;
	double t0 = now(), busy = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	ring_fd = open(path, O_RDWR);
	if (ring_fd < 0) {
		perror("glsrv: open ring");
		return 1;
	}
	ring_base = base;
	setword(1, 0);
	setword(2, 0);
	setword(3, 0);
	setword(0, GL_MAGIC);
	printf("glsrv: ready\n");
	for (;;) {
		uint32_t req = word(1);

		if (req == last) {
			usleep(300);
			continue;
		}
		double ts = now();
		uint32_t n = 0;

		/*
		 * The request counter can be seen before the batch has all landed (two devices, no ordering between them): a batch whose
		 * records do not add up to its length is read again, a little later.
		 */
		for (int tries = 0; tries < 200; tries++) {
			n = word(4);
			if (n > GL_PAYLOAD_MAX) {
				n = GL_PAYLOAD_MAX;
			}
			rd(GL_PAYLOAD_OFF, batch, n);
			size_t chk = 0;
			int ok = 1;

			while (chk + 12 <= n) {
				uint32_t nargs_c, bloblen_c;

				memcpy(&nargs_c, batch + chk + 4, 4);
				memcpy(&bloblen_c, batch + chk + 8, 4);
				size_t rec_c = 12 + (size_t)nargs_c * 4 + ((bloblen_c + 3) & ~3u);

				if (chk + rec_c > n) {
					ok = 0;
					break;
				}
				chk += rec_c;
			}
			if (ok && chk == n) {
				break;
			}
			if (tries == 199) {
				printf("glsrv: batch of %u bytes does not add up (%zu), used as it is\n", n, chk);
			}
			usleep(300);
		}
		reply_len = 0;
		result = 0;
		size_t pos = 0;

		while (pos + 12 <= n) {
			uint32_t op, nargs, bloblen;

			memcpy(&op, batch + pos, 4);
			memcpy(&nargs, batch + pos + 4, 4);
			memcpy(&bloblen, batch + pos + 8, 4);
			size_t rec = 12 + (size_t)nargs * 4 + ((bloblen + 3) & ~3u);

			if (pos + rec > n) {
				printf("glsrv: truncated record op %u nargs %u bloblen %u at %zu of %u (batch of req %u), last good op %u\n", op, nargs, bloblen, pos,
				    n, req, last_op);
				break;
			}
			last_op = op;
			if (ctx == EGL_NO_CONTEXT && op != OP_CREATE_CTX) {
				make_context();
			}
			double t_op = now();

			do_op(op, (const uint32_t *)(batch + pos + 12), nargs, batch + pos + 12 + nargs * 4, bloblen);
			if (op < 128) {
				op_time[op] += now() - t_op;
				op_count[op]++;
			}
			ops++;
			pos += rec;
		}
		uint32_t err = ctx == EGL_NO_CONTEXT ? 0 : glGetError();

		if (reply_len) {
			wr(GL_PAYLOAD_OFF, reply, reply_len);
		}
		setword(3, 0);
		setword(5, result);
		setword(6, reply_len);
		setword(7, err);
		last = req;
		setword(2, req);
		busy += now() - ts;
		if (++calls % 200 == 0) {
			char top[300];
			int k = 0;

			for (int i = 0; i < 128 && k < 260; i++) {
				if (op_time[i] > 1.0 || (i >= 105 && i <= 114 && op_count[i])) {
					k += snprintf(top + k, sizeof top - k, " op%d:%.1fs/%lu", i, op_time[i], op_count[i]);
				}
			}
			top[k] = 0;
			printf("glsrv: %lu batches, %lu ops, busy %.1f of %.1f s;%s\n", calls, ops, busy, now() - t0, top);
		}
	}
}
