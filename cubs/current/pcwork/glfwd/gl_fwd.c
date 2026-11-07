/*
 * The OpenGL ES library behind src/eagl.c when the GPU is on the other side of the shared ring (see gl_fwd.h): the gl* functions
 * CoreAnimation's OpenGL ES renderer and eagl.c use write records into a batch, sent to glsrv (Android, EGL/GLES on the phone's
 * PowerVR) when something is to be read back or a frame ends (glFlush, glFinish). Object names are the server's.
 * Used in place of gl_osmesa.c and libOSMesa: userland/build.sh GL_FWD=1 (patched), see pcwork/glfwd/build_gl.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "gl_fwd.h"
#include "redwood_gl.h"

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte, GLboolean;
typedef char GLchar;
typedef intptr_t GLintptr, GLsizeiptr;
typedef struct __GLsync *GLsync;

static int fd = -1;
static uint32_t seq;
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static uint8_t batch[GL_PAYLOAD_MAX];
static size_t blen;
static uint8_t replybuf[GL_PAYLOAD_MAX];
static uint32_t reply_n, result_w, last_err;
static unsigned long n_flush, n_ops, sync_ops[128], flush_why[8], rb_stat[8], up_stat[8];
static int why;
static double t_wait;

static void
say(const char *format, ...)
{
	char line[600];
	va_list ap;
	int con = open("/dev/console", O_WRONLY);

	va_start(ap, format);
	int n = vsnprintf(line, sizeof line, format, ap);
	va_end(ap);
	if (con >= 0) {
		if (n > 0) {
			(void)write(con, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
		}
		close(con);
	}
}

static uint32_t
word(int i)
{
	uint32_t v = 0;

	(void)pread(fd, &v, 4, GL_OFF + i * 4);
	return v;
}

static void
setword(int i, uint32_t v)
{
	(void)pwrite(fd, &v, 4, GL_OFF + i * 4);
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static int
connect_server(void)
{
	if (fd >= 0) {
		return 1;
	}
	fd = open("/dev/cubs-gpu", O_RDWR);
	if (fd < 0) {
		say("redwood-gl: forward: /dev/cubs-gpu: %d\n", errno);
		return 0;
	}
	for (int i = 0; i < 600; i++) {
		if (word(0) == GL_MAGIC) {
			seq = word(1);
			say("redwood-gl: forward: server found (counter %u)\n", seq);
			return 1;
		}
		usleep(100000);
	}
	say("redwood-gl: forward: no glsrv in the ring after 60 s\n");
	close(fd);
	fd = -1;
	return 0;
}

/* send the batch and wait for the server */
static void
flush(void)
{
	if (blen == 0) {
		return;
	}
	if (!connect_server()) {
		blen = 0;
		return;
	}
	(void)pwrite(fd, batch, blen, GL_PAYLOAD_OFF);
	setword(4, (uint32_t)blen);
	seq++;
	setword(1, seq);
	double t = now();

	while (word(2) != seq) {
		if (now() - t > 20) {
			say("redwood-gl: forward: the server did not answer (batch %zu bytes)\n", blen);
			blen = 0;
			return;
		}
		usleep(100);
	}
	t_wait += now() - t;
	result_w = word(5);
	reply_n = word(6);
	if (reply_n > sizeof replybuf) {
		reply_n = sizeof replybuf;
	}
	uint32_t err = word(7);

	if (err != 0 && last_err == 0) {
		last_err = err;
	}
	if (reply_n) {
		(void)pread(fd, replybuf, reply_n, GL_PAYLOAD_OFF);
	}
	n_flush++;
	flush_why[why & 7]++;
	why = 0;
	blen = 0;
	if (n_flush % 500 == 0) {
		char h[400];
		int k = 0;

		for (int i = 0; i < 128 && k < 360; i++) {
			if (sync_ops[i] > 100) {
				k += snprintf(h + k, sizeof h - k, " %d:%lu", i, sync_ops[i]);
			}
		}
		h[k] = 0;
		say("redwood-gl: forward: %lu batches, %lu ops, waited %.1f s; why s/fl/err/ovf %lu/%lu/%lu/%lu; rb new/valid/invalid/delta/full %lu/%lu/%lu/%lu/%lu; up other/rendered/new/delta %lu/%lu/%lu/%lu; sync ops%s\n", n_flush, n_ops, t_wait, flush_why[0], flush_why[1], flush_why[2], flush_why[3], rb_stat[0], rb_stat[1], rb_stat[2], rb_stat[3], rb_stat[4], up_stat[0], up_stat[1], up_stat[2], up_stat[3], h);
	}
}

static void
rec(uint32_t op, const uint32_t *args, uint32_t nargs, const void *blob, uint32_t bloblen)
{
	size_t size = 12 + (size_t)nargs * 4 + ((bloblen + 3) & ~3u);

	if (size > sizeof batch) {
		say("redwood-gl: forward: record of %zu bytes (op %u) is too big\n", size, op);
		return;
	}
	if (blen + size > sizeof batch) {
		why = 3;
		flush();
	}
	uint8_t *p = batch + blen;

	memcpy(p, &op, 4);
	memcpy(p + 4, &nargs, 4);
	memcpy(p + 8, &bloblen, 4);
	if (nargs) {
		memcpy(p + 12, args, (size_t)nargs * 4);
	}
	if (bloblen) {
		memcpy(p + 12 + nargs * 4, blob, bloblen);
		if (bloblen & 3) {
			memset(p + 12 + nargs * 4 + bloblen, 0, 4 - (bloblen & 3));
		}
	}
	blen += size;
	n_ops++;
}

/* a call that returns something: sent at once, the reply in replybuf/result_w */
static void
rec_sync(uint32_t op, const uint32_t *args, uint32_t nargs, const void *blob, uint32_t bloblen)
{
	reply_n = 0;
	result_w = 0;
	if (op < 128) {
		sync_ops[op]++;
	}
	rec(op, args, nargs, blob, bloblen);
	flush();
}

static uint32_t
f2u(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

#define A(...) ((const uint32_t[]){ __VA_ARGS__ })
#define N(...) (sizeof((const uint32_t[]){ __VA_ARGS__ }) / 4)
#define LOCK pthread_mutex_lock(&lock)
#define UNLOCK pthread_mutex_unlock(&lock)
#define SIMPLE(op, ...) do { LOCK; rec(op, A(__VA_ARGS__), N(__VA_ARGS__), NULL, 0); UNLOCK; } while (0)

/* ------------------------------------------------------------------------------------------------------------------ the
 * interface of redwood_gl.h */

struct redwood_gl {
	int version;
};

struct redwood_gl *
redwood_gl_create(int version)
{
	static struct redwood_gl the_context;

	LOCK;
	rec_sync(OP_CREATE_CTX, NULL, 0, NULL, 0);
	int ok = result_w != 0;

	UNLOCK;
	if (!ok) {
		say("redwood-gl: forward: the server has no context\n");
		return NULL;
	}
	the_context.version = version;
	return &the_context;
}

int
redwood_gl_make_current(struct redwood_gl *gl)
{
	(void)gl;
	return 1;
}

void
redwood_gl_destroy(struct redwood_gl *gl)
{
	(void)gl;
}

const char *
redwood_gl_library(void)
{
	return "OpenGL ES forwarded to the phone's GPU (glsrv)";
}

int
redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface)
{
	(void)gl;
	(void)surface;
	return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ state kept here */

static GLint unpack_alignment = 4, unpack_row_length, pack_alignment = 4, pack_row_length;

static struct {
	GLenum cap;
	int on;
} enables[] = {
	{ 0x0BE2, 0 }, { 0x0B71, 0 }, { 0x0C11, 0 }, { 0x0B44, 0 }, { 0x0B90, 0 }, { 0x0BD0, 1 }, { 0x8037, 0 }, { 0x809E, 0 }, { 0x80A0, 0 },
};

static struct {
	int enabled, size, type, normalized, stride;
	const void *ptr;
} attr[16];

static unsigned
type_size(GLenum type)
{
	switch (type) {
	case 0x1400: case 0x1401: return 1;
	case 0x1402: case 0x1403: case 0x140B: return 2;
	default: return 4;
	}
}

static unsigned
pixel_bytes(GLenum format, GLenum type)
{
	unsigned comps;

	switch (type) {
	case 0x8363: case 0x8033: case 0x8034: return 2;
	case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: return 4;
	case 0x8DAD: return 8;
	}
	switch (format) {
	case 0x1908: case 0x8D99: case 0x80E1: comps = 4; break;
	case 0x1907: case 0x8D98: comps = 3; break;
	case 0x190A: case 0x8227: case 0x8228: comps = 2; break;
	default: comps = 1; break;
	}
	return comps * type_size(type);
}

static size_t
row_stride(GLsizei w, unsigned bpp, GLint align, GLint row_length)
{
	size_t row = (size_t)(row_length > 0 ? row_length : w) * bpp;

	if (align > 1) {
		row = (row + align - 1) / align * align;
	}
	return row;
}

/* ------------------------------------------------------------------------------------------------------------------ gl* */

#define EXPORT __attribute__((visibility("default")))

#define TEXSHADOW 4096
#define DELTA_MIN (256 * 1024)
#define DELTA_BAND 32
static int active_unit;
static GLuint bound_tex[32];
static uint8_t rendered[TEXSHADOW];     /* drawn into (attached to a framebuffer, copied into): its contents are not only what was uploaded */
static struct {
	uint8_t *data;
	size_t size;
	GLsizei w, h;
	GLenum fmt, type;
	GLint ifmt;
} ushadow[TEXSHADOW];

EXPORT void glActiveTexture(GLenum t) { active_unit = (int)(t - 0x84C0) & 31; SIMPLE(OP_ACTIVE_TEXTURE, t); }
EXPORT void glBindTexture(GLenum t, GLuint id) { if (t == 0x0DE1) { bound_tex[active_unit] = id; } SIMPLE(OP_BIND_TEXTURE, t, id); }
EXPORT void glBlendEquation(GLenum e) { SIMPLE(OP_BLEND_EQ, e); }
EXPORT void glBlendFunc(GLenum s, GLenum d) { SIMPLE(OP_BLEND_FUNC, s, d); }
EXPORT void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { SIMPLE(OP_BLEND_FUNC_SEP, a, b, c, d); }
EXPORT void glClear(GLbitfield m) { SIMPLE(OP_CLEAR, m); }
static GLfloat clear_state[4];          /* (OpenGL starts with 0, 0, 0, 0) */

EXPORT void
glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	clear_state[0] = r;
	clear_state[1] = g;
	clear_state[2] = b;
	clear_state[3] = a;
	SIMPLE(OP_CLEAR_COLOR, f2u(r), f2u(g), f2u(b), f2u(a));
}
EXPORT void glClearStencil(GLint s) { SIMPLE(OP_CLEAR_STENCIL, (uint32_t)s); }
EXPORT void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	if (t == 0x0DE1 && bound_tex[active_unit] < TEXSHADOW) {
		rendered[bound_tex[active_unit]] = 1;
	}
	SIMPLE(OP_COPY_TEX_SUB_IMAGE_2D, t, (uint32_t)l, (uint32_t)xo, (uint32_t)yo, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}
EXPORT void glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
	SIMPLE(OP_BLIT_FRAMEBUFFER, (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d, (uint32_t)e, (uint32_t)f, (uint32_t)g, (uint32_t)h, m, fl);
}
static GLint vp_state[4], sc_state[4];
static int vp_known, sc_known;
static GLuint cur_program;

EXPORT void
glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	sc_state[0] = x;
	sc_state[1] = y;
	sc_state[2] = w;
	sc_state[3] = h;
	sc_known = 1;
	SIMPLE(OP_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

EXPORT void
glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	vp_state[0] = x;
	vp_state[1] = y;
	vp_state[2] = w;
	vp_state[3] = h;
	vp_known = 1;
	SIMPLE(OP_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}
EXPORT void glTexParameteri(GLenum t, GLenum p, GLint v) { SIMPLE(OP_TEX_PARAM_I, t, p, (uint32_t)v); }
EXPORT void glTexParameterf(GLenum t, GLenum p, GLfloat v) { SIMPLE(OP_TEX_PARAM_F, t, p, f2u(v)); }
EXPORT void glGenerateMipmap(GLenum t) { SIMPLE(OP_GEN_MIPMAP, t); }
EXPORT void glUseProgram(GLuint p) { cur_program = p; SIMPLE(OP_USE_PROGRAM, p); }
EXPORT void glAttachShader(GLuint p, GLuint s) { SIMPLE(OP_ATTACH_SHADER, p, s); }
EXPORT void glLinkProgram(GLuint p) { SIMPLE(OP_LINK_PROGRAM, p); }
EXPORT void glDeleteShader(GLuint s) { SIMPLE(OP_DELETE_SHADER, s); }
EXPORT void glDeleteProgram(GLuint p) { SIMPLE(OP_DELETE_PROGRAM, p); }
EXPORT void glCompileShader(GLuint s) { SIMPLE(OP_COMPILE_SHADER, s); }
EXPORT void glUniform1f(GLint l, GLfloat a) { SIMPLE(OP_UNIFORM1F, (uint32_t)l, f2u(a)); }
EXPORT void glUniform2f(GLint l, GLfloat a, GLfloat b) { SIMPLE(OP_UNIFORM2F, (uint32_t)l, f2u(a), f2u(b)); }
EXPORT void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { SIMPLE(OP_UNIFORM4F, (uint32_t)l, f2u(a), f2u(b), f2u(c), f2u(d)); }
EXPORT void glUniform1i(GLint l, GLint v) { SIMPLE(OP_UNIFORM1I, (uint32_t)l, (uint32_t)v); }
static GLuint cur_read_fbo, cur_draw_fbo, fbo_color[TEXSHADOW];

EXPORT void
glBindFramebuffer(GLenum t, GLuint id)
{
	if (t == 0x8D40) {
		cur_draw_fbo = cur_read_fbo = id;
	} else if (t == 0x8CA9) {
		cur_draw_fbo = id;
	} else if (t == 0x8CA8) {
		cur_read_fbo = id;
	}
	SIMPLE(OP_BIND_FRAMEBUFFER, t, id);
}
EXPORT void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint level)
{
	if (tex < TEXSHADOW && t != 0x8CA8) {          /* (a read framebuffer does not change what is in it) */
		rendered[tex] = 1;
	}
	if (a == 0x8CE0) {
		GLuint f = t == 0x8CA8 ? cur_read_fbo : cur_draw_fbo;

		if (f < TEXSHADOW) {
			fbo_color[f] = tex;
		}
	}
	SIMPLE(OP_FRAMEBUFFER_TEX2D, t, a, tt, tex, (uint32_t)level);
}

EXPORT void
glEnable(GLenum cap)
{
	for (unsigned i = 0; i < sizeof enables / sizeof enables[0]; i++) {
		if (enables[i].cap == cap) {
			enables[i].on = 1;
		}
	}
	SIMPLE(OP_ENABLE, cap);
}

EXPORT void
glDisable(GLenum cap)
{
	for (unsigned i = 0; i < sizeof enables / sizeof enables[0]; i++) {
		if (enables[i].cap == cap) {
			enables[i].on = 0;
		}
	}
	SIMPLE(OP_DISABLE, cap);
}

EXPORT GLboolean
glIsEnabled(GLenum cap)
{
	for (unsigned i = 0; i < sizeof enables / sizeof enables[0]; i++) {
		if (enables[i].cap == cap) {
			return enables[i].on;
		}
	}
	LOCK;
	rec_sync(OP_IS_ENABLED, A(cap), 1, NULL, 0);
	GLboolean r = result_w != 0;

	UNLOCK;
	return r;
}

EXPORT void
glPixelStorei(GLenum p, GLint v)
{
	switch (p) {
	case 0x0CF5: unpack_alignment = v; break;
	case 0x0CF2: unpack_row_length = v; break;
	case 0x0D05: pack_alignment = v; break;
	case 0x0D02: pack_row_length = v; break;
	}
	SIMPLE(OP_PIXEL_STORE, p, (uint32_t)v);
}

EXPORT void
glFlush(void)
{
	LOCK;
	rec(OP_FLUSH, NULL, 0, NULL, 0);
	why = 1;
	flush();
	UNLOCK;
}

EXPORT void
glFinish(void)
{
	/* not waited for on the GPU: what follows (a readback) does that, and glFlush's round trip already makes the server take the batch */
	LOCK;
	rec(OP_FLUSH, NULL, 0, NULL, 0);
	why = 1;
	flush();
	UNLOCK;
}

EXPORT void glFinishObjectAPPLE(GLenum o, GLint n) { (void)o; (void)n; glFinish(); }
EXPORT void glInsertEventMarkerEXT(GLsizei l, const char *m) { (void)l; (void)m; }
EXPORT void glPushGroupMarkerEXT(GLsizei l, const char *m) { (void)l; (void)m; }
EXPORT void glPopGroupMarkerEXT(void) { }
EXPORT void glFramebufferParameteriAPPLE(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
EXPORT void glLabelObjectEXT(GLenum t, GLuint o, GLsizei l, const char *s) { (void)t; (void)o; (void)l; (void)s; }
EXPORT void glAlphaFunc(GLenum f, GLfloat r) { (void)f; (void)r; }

EXPORT GLsync
glFenceSync(GLenum c, GLbitfield f)
{
	(void)c;
	(void)f;
	glFinish();
	return (GLsync)(uintptr_t)1;
}

EXPORT GLenum glClientWaitSync(GLsync s, GLbitfield f, uint64_t t) { (void)s; (void)f; (void)t; return 0x911A; }
EXPORT void glDeleteSync(GLsync s) { (void)s; }

EXPORT GLenum
glGetError(void)
{
	GLenum e;

	LOCK;
	/*
	 * No flush: eagl.c asks after every call it passes on, and a round trip each would be the whole cost. What the server saw
	 * (glGetError after a batch) is the answer, a batch late.
	 */
	e = last_err;
	last_err = 0;
	UNLOCK;
	return e;
}

EXPORT void
glGenTextures(GLsizei n, GLuint *ids)
{
	/* one round trip each, and not pooled: the server hands a freed name out again at once, which is what keeps the names of the textures
	 * and framebuffers of one frame the same as the frame before's (the readbacks' shadows are keyed by them) */
	LOCK;
	for (GLsizei done = 0; done < n;) {
		GLsizei k = n - done > 64 ? 64 : n - done;

		rec_sync(OP_GEN_TEXTURES, A((uint32_t)k), 1, NULL, 0);
		memcpy(ids + done, replybuf, reply_n < (uint32_t)k * 4 ? reply_n : (uint32_t)k * 4);
		done += k;
	}
	UNLOCK;
}

EXPORT void
glGenFramebuffers(GLsizei n, GLuint *ids)
{
	/* one round trip each, and not pooled: the server hands a freed name out again at once, which is what keeps the names of the textures
	 * and framebuffers of one frame the same as the frame before's (the readbacks' shadows are keyed by them) */
	LOCK;
	for (GLsizei done = 0; done < n;) {
		GLsizei k = n - done > 64 ? 64 : n - done;

		rec_sync(OP_GEN_FRAMEBUFFERS, A((uint32_t)k), 1, NULL, 0);
		memcpy(ids + done, replybuf, reply_n < (uint32_t)k * 4 ? reply_n : (uint32_t)k * 4);
		done += k;
	}
	UNLOCK;
}

EXPORT void
glDeleteTextures(GLsizei n, const GLuint *ids)
{
	LOCK;
	for (GLsizei i = 0; i < n; i++) {
		if (ids[i] < TEXSHADOW) {
			free(ushadow[ids[i]].data);
			memset(&ushadow[ids[i]], 0, sizeof ushadow[ids[i]]);
			rendered[ids[i]] = 0;
		}
	}
	for (GLsizei done = 0; done < n;) {
		uint32_t a[65];
		GLsizei k = n - done > 64 ? 64 : n - done;

		a[0] = (uint32_t)k;
		memcpy(a + 1, ids + done, (size_t)k * 4);
		rec(OP_DELETE_TEXTURES, a, 1 + k, NULL, 0);
		done += k;
	}
	UNLOCK;
}

EXPORT void
glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
	LOCK;
	for (GLsizei done = 0; done < n;) {
		uint32_t a[65];
		GLsizei k = n - done > 64 ? 64 : n - done;

		a[0] = (uint32_t)k;
		memcpy(a + 1, ids + done, (size_t)k * 4);
		rec(OP_DELETE_FRAMEBUFFERS, a, 1 + k, NULL, 0);
		done += k;
	}
	UNLOCK;
}

EXPORT GLuint
glCreateShader(GLenum type)
{
	LOCK;
	rec_sync(OP_CREATE_SHADER, A(type), 1, NULL, 0);
	GLuint r = result_w;

	UNLOCK;
	return r;
}

EXPORT GLuint
glCreateProgram(void)
{
	LOCK;
	rec_sync(OP_CREATE_PROGRAM, NULL, 0, NULL, 0);
	GLuint r = result_w;

	UNLOCK;
	return r;
}

EXPORT void
glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	size_t total = 0;

	for (GLsizei i = 0; i < count; i++) {
		total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	char *src = malloc(total + 1);

	if (!src) {
		return;
	}
	size_t pos = 0;

	for (GLsizei i = 0; i < count; i++) {
		size_t l = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);

		memcpy(src + pos, strings[i], l);
		pos += l;
	}
	src[total] = 0;
	if (strstr(src, "#version 300")) {
		/* OpenGL ES 3 shaders have derivatives in the language: ANGLE refuses the extension line (same length: "//xtension") */
		char *e;

		while ((e = strstr(src, "#extension GL_OES_standard_derivatives")) != NULL) {
			memcpy(e, "//xtension", 10);
		}
	}
	LOCK;
	rec(OP_SHADER_SOURCE, A(shader, (uint32_t)total), 2, src, (uint32_t)total);
	UNLOCK;
	free(src);
}

EXPORT void
glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
	LOCK;
	rec(OP_BIND_ATTRIB_LOC, A(program, index, (uint32_t)strlen(name)), 3, name, (uint32_t)strlen(name));
	UNLOCK;
}

#define UNIFORM_CACHE 256
static struct {
	GLuint program;
	char name[48];
	GLint loc;
} ucache[UNIFORM_CACHE];

EXPORT GLint
glGetUniformLocation(GLuint program, const GLchar *name)
{
	size_t l = strlen(name);
	unsigned h = program * 2654435761u;

	for (size_t i = 0; i < l; i++) {
		h = h * 31 + (unsigned char)name[i];
	}
	unsigned slot = h % UNIFORM_CACHE;

	if (ucache[slot].program == program && l < sizeof ucache[slot].name && strcmp(ucache[slot].name, name) == 0) {
		return ucache[slot].loc;
	}
	LOCK;
	rec_sync(OP_GET_UNIFORM_LOCATION, A(program, (uint32_t)l), 2, name, (uint32_t)l);
	GLint r = (GLint)result_w;

	UNLOCK;
	if (l < sizeof ucache[slot].name) {
		ucache[slot].program = program;
		strcpy(ucache[slot].name, name);
		ucache[slot].loc = r;
	}
	return r;
}

EXPORT void
glUniform1fv(GLint l, GLsizei c, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_VEC, A((uint32_t)l, (uint32_t)c, 1), 3, v, (uint32_t)c * 4);
	UNLOCK;
}

EXPORT void
glUniform3fv(GLint l, GLsizei c, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_VEC, A((uint32_t)l, (uint32_t)c, 3), 3, v, (uint32_t)c * 12);
	UNLOCK;
}

EXPORT void
glUniform4fv(GLint l, GLsizei c, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_VEC, A((uint32_t)l, (uint32_t)c, 4), 3, v, (uint32_t)c * 16);
	UNLOCK;
}

EXPORT void
glUniform2fv(GLint l, GLsizei c, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_VEC, A((uint32_t)l, (uint32_t)c, 2), 3, v, (uint32_t)c * 8);
	UNLOCK;
}

EXPORT void
glUniformMatrix3fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_MAT, A((uint32_t)l, (uint32_t)c, 3, t), 4, v, (uint32_t)c * 36);
	UNLOCK;
}

EXPORT void
glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v)
{
	LOCK;
	rec(OP_UNIFORM_MAT, A((uint32_t)l, (uint32_t)c, 4, t), 4, v, (uint32_t)c * 64);
	UNLOCK;
}

/* textures: pixels travel in the batch; what does not fit is sent in bands of rows */
#define BAND_BYTES 0x100000

EXPORT void
glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *pixels)
{
	static struct {
		GLsizei w, h;
		GLenum fmt, type;
		GLint ifmt;
		size_t stride, size;
		uint8_t *shadow;
		int have;
	} uc[4];
	static unsigned next_uc;
	unsigned bpp = pixel_bytes(fmt, type);
	size_t stride = row_stride(w, bpp, unpack_alignment, unpack_row_length);
	size_t size = stride * (size_t)h;

	LOCK;
	if (pixels && size >= DELTA_MIN && size <= ((size_t)32 << 20) && target == 0x0DE1) {
		/* a big image: the server keeps a copy per kind of image (size and format); only the bands of rows that differ from
		 * what it has are sent, and it makes the texture of its copy (textures of the same kind come again and again) */
		int e = -1;

		for (int i = 0; i < 4; i++) {
			if (uc[i].shadow && uc[i].w == w && uc[i].h == h && uc[i].fmt == fmt && uc[i].type == type && uc[i].stride == stride && uc[i].ifmt == ifmt) {
				e = i;
			}
		}
		if (e < 0) {
			e = (int)(next_uc++ & 3);
			free(uc[e].shadow);
			uc[e].shadow = malloc(size);
			uc[e].w = w;
			uc[e].h = h;
			uc[e].fmt = fmt;
			uc[e].type = type;
			uc[e].stride = stride;
			uc[e].size = size;
			uc[e].ifmt = ifmt;
			uc[e].have = 0;
			up_stat[2]++;
			rec(OP_CACHE_DEFINE, A((uint32_t)e, (uint32_t)w, (uint32_t)h, (uint32_t)ifmt, fmt, type), 6, NULL, 0);
		}
		if (uc[e].shadow) {
			for (size_t y = 0; y < (size_t)h; y += DELTA_BAND * 4) {
				size_t k = (size_t)h - y < DELTA_BAND * 4 ? (size_t)h - y : DELTA_BAND * 4;
				const uint8_t *from = (const uint8_t *)pixels + y * stride;

				if (!uc[e].have || memcmp(from, uc[e].shadow + y * stride, stride * k) != 0) {
					rec(OP_CACHE_PATCH, A((uint32_t)e, (uint32_t)y, (uint32_t)k, (uint32_t)stride, (uint32_t)size), 5, from, (uint32_t)(stride * k));
					memcpy(uc[e].shadow + y * stride, from, stride * k);
					up_stat[3]++;
				}
			}
			uc[e].have = 1;
			rec(OP_TEX_IMAGE_FROM_CACHE, A((uint32_t)e, target, (uint32_t)level, (uint32_t)ifmt, (uint32_t)w, (uint32_t)h, (uint32_t)border, fmt, type), 9,
			    NULL, 0);
			UNLOCK;
			return;
		}
	}
	if (!pixels || size == 0) {
		rec(OP_TEX_IMAGE_2D, A(target, (uint32_t)level, (uint32_t)ifmt, (uint32_t)w, (uint32_t)h, (uint32_t)border, fmt, type, 0), 9, NULL, 0);
	} else if (size <= BAND_BYTES) {
		rec(OP_TEX_IMAGE_2D, A(target, (uint32_t)level, (uint32_t)ifmt, (uint32_t)w, (uint32_t)h, (uint32_t)border, fmt, type, 1), 9, pixels,
		    (uint32_t)size);
	} else {
		size_t rows = BAND_BYTES / stride ? BAND_BYTES / stride : 1;

		rec(OP_TEX_IMAGE_2D, A(target, (uint32_t)level, (uint32_t)ifmt, (uint32_t)w, (uint32_t)h, (uint32_t)border, fmt, type, 0), 9, NULL, 0);
		for (size_t y = 0; y < (size_t)h; y += rows) {
			size_t k = (size_t)h - y < rows ? (size_t)h - y : rows;

			rec(OP_TEX_SUB_IMAGE_2D, A(target, (uint32_t)level, 0, (uint32_t)y, (uint32_t)w, (uint32_t)k, fmt, type), 8,
			    (const uint8_t *)pixels + y * stride, (uint32_t)(stride * k));
		}
	}
	UNLOCK;
}

EXPORT void
glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *pixels)
{
	unsigned bpp = pixel_bytes(fmt, type);
	size_t stride = row_stride(w, bpp, unpack_alignment, unpack_row_length);
	size_t rows = BAND_BYTES / stride ? BAND_BYTES / stride : 1;

	if (!pixels) {
		return;
	}
	LOCK;
	for (size_t y = 0; y < (size_t)h; y += rows) {
		size_t k = (size_t)h - y < rows ? (size_t)h - y : rows;

		rec(OP_TEX_SUB_IMAGE_2D, A(target, (uint32_t)level, (uint32_t)xo, (uint32_t)(yo + (GLint)y), (uint32_t)w, (uint32_t)k, fmt, type), 8,
		    (const uint8_t *)pixels + y * stride, (uint32_t)(stride * k));
	}
	UNLOCK;
}

EXPORT void
glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data)
{
	LOCK;
	if ((size_t)size <= BAND_BYTES * 2) {
		rec(OP_COMPRESSED_TEX_IMAGE_2D, A(target, (uint32_t)level, ifmt, (uint32_t)w, (uint32_t)h, (uint32_t)border, (uint32_t)size), 7, data,
		    data ? (uint32_t)size : 0);
	} else {
		say("redwood-gl: forward: compressed texture of %d bytes not sent\n", size);
	}
	UNLOCK;
}

EXPORT void
glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *pixels)
{
	static struct {
		void *dest;
		GLint x, y;
		GLsizei w, h;
		GLenum fmt, type;
		size_t stride;
		GLuint tex;
		uint8_t *shadow;
		int have;
	} rb[8];
	static unsigned next_rb;
	unsigned bpp = pixel_bytes(fmt, type);
	size_t stride = row_stride(w, bpp, pack_alignment, pack_row_length);
	size_t size = stride * (size_t)h;
	size_t rows = BAND_BYTES / stride ? BAND_BYTES / stride : 1;
	int entry = -1;

	LOCK;
	if (size >= DELTA_MIN && size <= ((size_t)32 << 20)) {
		GLuint rtex = cur_read_fbo < TEXSHADOW ? fbo_color[cur_read_fbo] : 0;

		for (int i = 0; i < 8; i++) {
			if (rb[i].shadow && rb[i].tex == rtex && rb[i].x == x && rb[i].y == y && rb[i].w == w && rb[i].h == h && rb[i].fmt == fmt &&
			    rb[i].type == type && rb[i].stride == stride) {
				entry = i;
			}
		}
		if (entry < 0) {
			rb_stat[0]++;
			entry = (int)(next_rb++ & 7);
			free(rb[entry].shadow);
			rb[entry].shadow = malloc(size);
			rb[entry].tex = rtex;
			rb[entry].dest = pixels;
			rb[entry].x = x;
			rb[entry].y = y;
			rb[entry].w = w;
			rb[entry].h = h;
			rb[entry].fmt = fmt;
			rb[entry].type = type;
			rb[entry].stride = stride;
			rb[entry].have = 0;
		}
		if (rb[entry].shadow) {
			int valid = rb[entry].have;      /* (the destination is rebuilt from the shadow below: its own contents do not matter) */

			rb_stat[valid ? 1 : 2]++;

			rec_sync(OP_READ_PIXELS_DELTA, A((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fmt, type, (uint32_t)stride, (uint32_t)entry,
			    (uint32_t)valid, 0u), 10, NULL, 0);
			uint32_t mode = 0, nb = 0, more = 0;

			if (reply_n >= 12) {
				memcpy(&mode, replybuf, 4);
				memcpy(&nb, replybuf + 4, 4);
				memcpy(&more, replybuf + 8, 4);
			}
			rb_stat[mode == 1 ? 3 : 4]++;
			if (mode == 1 && valid) {
				for (;;) {
					size_t pos = 12;

					for (uint32_t i = 0; i < nb && pos + 8 <= reply_n; i++) {
						uint32_t row0, nrows;

						memcpy(&row0, replybuf + pos, 4);
						memcpy(&nrows, replybuf + pos + 4, 4);
						pos += 8;
						if (pos + (size_t)nrows * stride > reply_n || (size_t)row0 + nrows > (size_t)h) {
							break;
						}
						memcpy(rb[entry].shadow + (size_t)row0 * stride, replybuf + pos, (size_t)nrows * stride);
						pos += (size_t)nrows * stride;
					}
					if (!more) {
						break;
					}
					rec_sync(OP_READ_PIXELS_DELTA, A((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fmt, type, (uint32_t)stride,
					    (uint32_t)entry, (uint32_t)valid, 1u), 10, NULL, 0);
					nb = more = 0;
					if (reply_n >= 12) {
						memcpy(&nb, replybuf + 4, 4);
						memcpy(&more, replybuf + 8, 4);
					}
				}
				memcpy(pixels, rb[entry].shadow, size);
				rb[entry].have = 1;
				UNLOCK;
				return;
			}
			/* everything: the server has kept this readback to compare with the next, so only the bands are read now */
			for (size_t yy = 0; yy < (size_t)h; yy += rows) {
				size_t k = (size_t)h - yy < rows ? (size_t)h - yy : rows;

				rec_sync(OP_READ_PIXELS, A((uint32_t)x, (uint32_t)(y + (GLint)yy), (uint32_t)w, (uint32_t)k, fmt, type, (uint32_t)(stride * k)), 7,
				    NULL, 0);
				memcpy(rb[entry].shadow + yy * stride, replybuf, reply_n < stride * k ? reply_n : stride * k);
			}
			/* (into our own copy first and into the destination at once: whoever looks at the surface -- the display's driver -- must
			 * not see a frame that is half the new one) */
			memcpy(pixels, rb[entry].shadow, size);
			rb[entry].have = 1;
			UNLOCK;
			return;
		}
	}
	for (size_t yy = 0; yy < (size_t)h; yy += rows) {
		size_t k = (size_t)h - yy < rows ? (size_t)h - yy : rows;

		rec_sync(OP_READ_PIXELS, A((uint32_t)x, (uint32_t)(y + (GLint)yy), (uint32_t)w, (uint32_t)k, fmt, type, (uint32_t)(stride * k)), 7, NULL, 0);
		memcpy((uint8_t *)pixels + yy * stride, replybuf, reply_n < stride * k ? reply_n : stride * k);
	}
	UNLOCK;
}

/* vertex arrays live in client memory: sent with every draw */
EXPORT void
glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *ptr)
{
	if (index < 16) {
		attr[index].size = size;
		attr[index].type = type;
		attr[index].normalized = normalized;
		attr[index].stride = stride;
		attr[index].ptr = ptr;
	}
}

EXPORT void glEnableVertexAttribArray(GLuint i) { if (i < 16) { attr[i].enabled = 1; } }
EXPORT void glDisableVertexAttribArray(GLuint i) { if (i < 16) { attr[i].enabled = 0; } }

/* the descriptors and bytes of the enabled arrays for vertices 0 .. last; first is added to the pointer first */
static size_t
arrays_for(uint32_t *desc, uint8_t *blob, size_t cap, size_t first, size_t count, int *nattr)
{
	size_t used = 0;
	int n = 0;

	for (int i = 0; i < 16; i++) {
		if (!attr[i].enabled || !attr[i].ptr) {
			continue;
		}
		size_t elem = (size_t)attr[i].size * type_size(attr[i].type);
		size_t stride = attr[i].stride ? (size_t)attr[i].stride : elem;
		size_t bytes = count ? (count - 1) * stride + elem : 0;

		if (used + ((bytes + 3) & ~(size_t)3) > cap) {
			say("redwood-gl: forward: vertex arrays of %zu bytes do not fit\n", used + bytes);
			break;
		}
		desc[n * 6 + 0] = (uint32_t)i;
		desc[n * 6 + 1] = (uint32_t)attr[i].size;
		desc[n * 6 + 2] = attr[i].type;
		desc[n * 6 + 3] = (uint32_t)attr[i].normalized;
		desc[n * 6 + 4] = (uint32_t)attr[i].stride;
		desc[n * 6 + 5] = (uint32_t)bytes;
		memcpy(blob + used, (const uint8_t *)attr[i].ptr + first * stride, bytes);
		used += (bytes + 3) & ~(size_t)3;
		n++;
	}
	*nattr = n;
	return used;
}

EXPORT void
glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	static uint32_t args[4 + 16 * 6];
	static uint8_t blob[0x180000];
	int nattr;

	LOCK;
	size_t used = arrays_for(args + 4, blob, sizeof blob, (size_t)first, (size_t)count, &nattr);

	args[0] = mode;
	args[1] = (uint32_t)first;
	args[2] = (uint32_t)count;
	args[3] = (uint32_t)nattr;
	rec(OP_DRAW_ARRAYS, args, 4 + nattr * 6, blob, (uint32_t)used);
	UNLOCK;
}

EXPORT void
glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	static uint32_t args[5 + 16 * 6];
	static uint8_t blob[0x180000];
	int nattr;
	size_t isz = type_size(type), ilen = (size_t)count * isz, maxv = 0, minv = (size_t)-1;

	if (!indices || ilen > 0x40000 || count <= 0) {
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		size_t v = isz == 1 ? ((const uint8_t *)indices)[i] : isz == 2 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i];

		if (v > maxv) {
			maxv = v;
		}
		if (v < minv) {
			minv = v;
		}
	}
	LOCK;
	/* the indices are sent rebased to the smallest, and only the arrays between the smallest and the largest */
	for (GLsizei i = 0; i < count; i++) {
		if (isz == 1) {
			blob[i] = (uint8_t)(((const uint8_t *)indices)[i] - minv);
		} else if (isz == 2) {
			((uint16_t *)blob)[i] = (uint16_t)(((const uint16_t *)indices)[i] - minv);
		} else {
			((uint32_t *)blob)[i] = (uint32_t)(((const uint32_t *)indices)[i] - minv);
		}
	}
	size_t off = (ilen + 3) & ~(size_t)3;
	size_t used = arrays_for(args + 5, blob + off, sizeof blob - off, minv, maxv - minv + 1, &nattr);

	args[0] = mode;
	args[1] = (uint32_t)count;
	args[2] = type;
	args[3] = (uint32_t)nattr;
	args[4] = (uint32_t)ilen;
	rec(OP_DRAW_ELEMENTS, args, 5 + nattr * 6, blob, (uint32_t)(off + used));
	UNLOCK;
}

/* ------------------------------------------------------------------------------------------------------------------ queries */

EXPORT const GLubyte *
glGetString(GLenum name)
{
	static char *cache[8];
	unsigned slot = name == 0x1F00 ? 0 : name == 0x1F01 ? 1 : name == 0x1F02 ? 2 : name == 0x1F03 ? 3 : name == 0x8B8C ? 4 : 7;

	if (cache[slot]) {
		return (const GLubyte *)cache[slot];
	}
	LOCK;
	rec_sync(OP_GET_STRING, A(name), 1, NULL, 0);
	cache[slot] = strndup((const char *)replybuf, reply_n);
	UNLOCK;
	return (const GLubyte *)cache[slot];
}

static int
static_pname(GLenum p)
{
	switch (p) {
	case 0x0D33: case 0x0D3A: case 0x8872: case 0x8869: case 0x8B4C: case 0x8B4D: case 0x84E8: case 0x851C: case 0x8DFB: case 0x8DFC:
	case 0x8B49: case 0x8B4A: case 0x8DFD: case 0x0D50: case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: case 0x0D56:
		return 1;
	}
	return 0;
}

EXPORT void
glGetIntegerv(GLenum pname, GLint *v)
{
	static GLint cached[0x10000][2];
	static unsigned char have[0x10000];
	static unsigned char counts[0x10000];

	switch (pname) {
	case 0x0CF5: *v = unpack_alignment; return;
	case 0x0CF2: *v = unpack_row_length; return;
	case 0x0D05: *v = pack_alignment; return;
	case 0x0D02: *v = pack_row_length; return;
	case 0x8069: *v = (GLint)bound_tex[active_unit]; return;
	case 0x84E0: *v = 0x84C0 + active_unit; return;
	case 0x8CA6: *v = (GLint)cur_draw_fbo; return;
	case 0x8CAA: *v = (GLint)cur_read_fbo; return;
	case 0x8B8D: *v = (GLint)cur_program; return;
	case 0x8894: case 0x8895: case 0x85B5: *v = 0; return;      /* (buffer and vertex array bindings: never bound here) */
	case 0x0BA2:
		if (vp_known) {
			memcpy(v, vp_state, 16);
			return;
		}
		break;
	case 0x0C10:
		if (sc_known) {
			memcpy(v, sc_state, 16);
			return;
		}
		break;
	}
	LOCK;
	if (static_pname(pname) && pname < 0x10000 && have[pname]) {
		memcpy(v, cached[pname], (size_t)counts[pname] * 4);
		UNLOCK;
		return;
	}
	rec_sync(OP_GET_INTEGERV, A(pname), 1, NULL, 0);
	uint32_t n = reply_n / 4;

	memcpy(v, replybuf, reply_n);
	if (static_pname(pname) && pname < 0x10000 && n && n <= 2) {
		memcpy(cached[pname], replybuf, reply_n);
		counts[pname] = (unsigned char)n;
		have[pname] = 1;
	}
	UNLOCK;
}

EXPORT void
glGetFloatv(GLenum pname, GLfloat *v)
{
	static GLenum fl_p[8];
	static unsigned long fl_c[8];

	if (pname == 0x0C22) {
		memcpy(v, clear_state, 16);
		return;
	}
	LOCK;
	for (int i = 0; i < 8; i++) {
		if (fl_c[i] == 0 || fl_p[i] == pname) {
			fl_p[i] = pname;
			if (++fl_c[i] % 5000 == 0) {
				say("redwood-gl: forward: glGetFloatv %#x asked %lu times\n", pname, fl_c[i]);
			}
			break;
		}
	}
	rec_sync(OP_GET_FLOATV, A(pname), 1, NULL, 0);
	memcpy(v, replybuf, reply_n);
	UNLOCK;
}

EXPORT void
glGetProgramiv(GLuint p, GLenum pname, GLint *v)
{
	LOCK;
	rec_sync(OP_GET_PROGRAMIV, A(p, pname), 2, NULL, 0);
	*v = (GLint)result_w;
	UNLOCK;
}

EXPORT void
glGetShaderiv(GLuint s, GLenum pname, GLint *v)
{
	LOCK;
	rec_sync(OP_GET_SHADERIV, A(s, pname), 2, NULL, 0);
	*v = (GLint)result_w;
	UNLOCK;
}

static void
info_log(uint32_t op, GLuint id, GLsizei bufsize, GLsizei *length, GLchar *log)
{
	LOCK;
	rec_sync(op, A(id), 1, NULL, 0);
	GLsizei n = (GLsizei)(reply_n ? reply_n - 1 : 0);

	if (n > bufsize - 1) {
		n = bufsize > 0 ? bufsize - 1 : 0;
	}
	if (bufsize > 0) {
		memcpy(log, replybuf, (size_t)n);
		log[n] = 0;
	}
	if (length) {
		*length = n;
	}
	UNLOCK;
}

EXPORT void glGetShaderInfoLog(GLuint s, GLsizei b, GLsizei *l, GLchar *log) { info_log(OP_GET_SHADER_INFO_LOG, s, b, l, log); }
EXPORT void glGetProgramInfoLog(GLuint p, GLsizei b, GLsizei *l, GLchar *log) { info_log(OP_GET_PROGRAM_INFO_LOG, p, b, l, log); }

EXPORT void
glGetTexLevelParameteriv(GLenum t, GLint level, GLenum pname, GLint *v)
{
	LOCK;
	rec_sync(OP_GET_TEXLEVELPARAM, A(t, (uint32_t)level, pname), 3, NULL, 0);
	*v = (GLint)result_w;
	UNLOCK;
}

EXPORT void
glGetTexParameteriv(GLenum t, GLenum pname, GLint *v)
{
	LOCK;
	rec_sync(OP_GET_TEXPARAM, A(t, pname), 2, NULL, 0);
	*v = (GLint)result_w;
	UNLOCK;
}

EXPORT void
glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum pname, GLint *v)
{
	if (a == 0x8CE0 && (pname == 0x8CD1 || pname == 0x8CD0)) {
		GLuint f = t == 0x8CA8 ? cur_read_fbo : cur_draw_fbo;

		if (f != 0 && f < TEXSHADOW && fbo_color[f] != 0) {
			*v = pname == 0x8CD1 ? (GLint)fbo_color[f] : 0x1702;
			return;
		}
	}
	LOCK;
	rec_sync(OP_GET_FB_ATTACH_PARAM, A(t, a, pname), 3, NULL, 0);
	*v = (GLint)result_w;
	UNLOCK;
}

EXPORT void
glGetActiveUniform(GLuint p, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
	LOCK;
	rec_sync(OP_GET_ACTIVE_UNIFORM, A(p, index, (uint32_t)bufsize), 3, NULL, 0);
	GLint sz = 0;
	GLenum ty = 0;

	if (reply_n >= 8) {
		memcpy(&sz, replybuf, 4);
		memcpy(&ty, replybuf + 4, 4);
	}
	GLsizei n = reply_n > 8 ? (GLsizei)(reply_n - 9) : 0;

	if (bufsize > 0) {
		if (n > bufsize - 1) {
			n = bufsize - 1;
		}
		memcpy(name, replybuf + 8, (size_t)n);
		name[n] = 0;
	}
	if (length) {
		*length = n;
	}
	if (size) {
		*size = sz;
	}
	if (type) {
		*type = ty;
	}
	UNLOCK;
}

EXPORT void
glGetAttachedShaders(GLuint p, GLsizei max, GLsizei *count, GLuint *shaders)
{
	LOCK;
	rec_sync(OP_GET_ATTACHED_SHADERS, A(p, (uint32_t)max), 2, NULL, 0);
	GLsizei n = 0;

	if (reply_n >= 4) {
		memcpy(&n, replybuf, 4);
		if (n > max) {
			n = max;
		}
		memcpy(shaders, replybuf + 4, (size_t)n * 4);
	}
	if (count) {
		*count = n;
	}
	UNLOCK;
}

EXPORT GLboolean
glIsTexture(GLuint t)
{
	LOCK;
	rec_sync(OP_IS_TEXTURE, A(t), 1, NULL, 0);
	GLboolean r = result_w != 0;

	UNLOCK;
	return r;
}

EXPORT GLboolean
glIsProgram(GLuint p)
{
	LOCK;
	rec_sync(OP_IS_PROGRAM, A(p), 1, NULL, 0);
	GLboolean r = result_w != 0;

	UNLOCK;
	return r;
}

EXPORT void
glGetUniformfv(GLuint p, GLint loc, GLfloat *v)
{
	LOCK;
	rec_sync(OP_GET_UNIFORMFV, A(p, (uint32_t)loc), 2, NULL, 0);
	memcpy(v, replybuf, reply_n < 16 * 4 ? reply_n : 16 * 4);
	UNLOCK;
}

EXPORT void
glGetUniformiv(GLuint p, GLint loc, GLint *v)
{
	LOCK;
	rec_sync(OP_GET_UNIFORMIV, A(p, (uint32_t)loc), 2, NULL, 0);
	memcpy(v, replybuf, reply_n < 16 * 4 ? reply_n : 16 * 4);
	UNLOCK;
}

EXPORT void
glGetVertexAttribiv(GLuint index, GLenum pname, GLint *v)
{
	*v = 0;
	if (index >= 16) {
		return;
	}
	switch (pname) {
	case 0x8622: *v = attr[index].enabled; break;
	case 0x8623: *v = attr[index].size; break;
	case 0x8624: *v = attr[index].stride; break;
	case 0x8625: *v = (GLint)attr[index].type; break;
	case 0x886A: *v = attr[index].normalized; break;
	}
}
