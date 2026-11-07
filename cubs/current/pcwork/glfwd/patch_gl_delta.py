#!/usr/bin/env python3
"""patch_gl_delta.py GL_FWD.C: send only what changed. Uploads of a big texture (glTexImage2D) keep a copy per texture and send
the changed bands of rows with glTexSubImage2D; readbacks (glReadPixels) are done in one round trip in which the server compares
with its previous readback of the same place and returns only the changed bands (the client checks that its destination memory
still holds what it was given last time)."""
import sys

path = sys.argv[1]
s = open(path).read()
if 'TEXSHADOW' in s:
    print('already patched')
    sys.exit(0)


def rep(old, new):
    global s
    assert old in s, old[:60]
    s = s.replace(old, new, 1)


rep('EXPORT void glActiveTexture(GLenum t) { SIMPLE(OP_ACTIVE_TEXTURE, t); }\nEXPORT void glBindTexture(GLenum t, GLuint id) { SIMPLE(OP_BIND_TEXTURE, t, id); }',
'''#define TEXSHADOW 4096
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
EXPORT void glBindTexture(GLenum t, GLuint id) { if (t == 0x0DE1) { bound_tex[active_unit] = id; } SIMPLE(OP_BIND_TEXTURE, t, id); }''')

rep('''EXPORT void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
''', '''EXPORT void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	if (t == 0x0DE1 && bound_tex[active_unit] < TEXSHADOW) {
		rendered[bound_tex[active_unit]] = 1;
	}
''')

rep('''EXPORT void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint level)
{
''', '''EXPORT void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint level)
{
	if (tex < TEXSHADOW) {
		rendered[tex] = 1;
	}
''')

rep('''glDeleteTextures(GLsizei n, const GLuint *ids)
{
	LOCK;
''', '''glDeleteTextures(GLsizei n, const GLuint *ids)
{
	LOCK;
	for (GLsizei i = 0; i < n; i++) {
		if (ids[i] < TEXSHADOW) {
			free(ushadow[ids[i]].data);
			memset(&ushadow[ids[i]], 0, sizeof ushadow[ids[i]]);
			rendered[ids[i]] = 0;
		}
	}
''')

# glTexImage2D: the whole function
a = s.index('glTexImage2D(GLenum target, GLint level, GLint ifmt')
b = s.index('\n}\n', a) + 3
new_tex = '''glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *pixels)
{
	unsigned bpp = pixel_bytes(fmt, type);
	size_t stride = row_stride(w, bpp, unpack_alignment, unpack_row_length);
	size_t size = stride * (size_t)h;
	GLuint tex = target == 0x0DE1 && level == 0 ? bound_tex[active_unit] : TEXSHADOW;

	LOCK;
	if (tex < TEXSHADOW && !(pixels && size >= DELTA_MIN && !rendered[tex])) {
		free(ushadow[tex].data);                /* (what is in the texture is not what the shadow says any more) */
		memset(&ushadow[tex], 0, sizeof ushadow[tex]);
	}
	if (tex < TEXSHADOW && pixels && size >= DELTA_MIN && !rendered[tex]) {
		uint8_t *old = ushadow[tex].data;

		if (old && ushadow[tex].size == size && ushadow[tex].w == w && ushadow[tex].h == h && ushadow[tex].fmt == fmt &&
		    ushadow[tex].type == type && ushadow[tex].ifmt == ifmt) {
			/* the same texture again: only the bands of rows that changed */
			for (size_t y = 0; y < (size_t)h; y += DELTA_BAND) {
				size_t k = (size_t)h - y < DELTA_BAND ? (size_t)h - y : DELTA_BAND;
				const uint8_t *from = (const uint8_t *)pixels + y * stride;

				if (memcmp(from, old + y * stride, stride * k) != 0) {
					rec(OP_TEX_SUB_IMAGE_2D, A(target, (uint32_t)level, 0, (uint32_t)y, (uint32_t)w, (uint32_t)k, fmt, type), 8, from,
					    (uint32_t)(stride * k));
					memcpy(old + y * stride, from, stride * k);
				}
			}
			UNLOCK;
			return;
		}
		free(old);
		ushadow[tex].data = malloc(size);
		if (ushadow[tex].data) {
			memcpy(ushadow[tex].data, pixels, size);
			ushadow[tex].size = size;
			ushadow[tex].w = w;
			ushadow[tex].h = h;
			ushadow[tex].fmt = fmt;
			ushadow[tex].type = type;
			ushadow[tex].ifmt = ifmt;
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
'''
s = s[:a] + new_tex + s[b:]

# glReadPixels: the whole function
a = s.index('glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *pixels)')
b = s.index('\n}\n', a) + 3
new_read = '''glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *pixels)
{
	static struct {
		void *dest;
		GLint x, y;
		GLsizei w, h;
		GLenum fmt, type;
		size_t stride;
		uint8_t *shadow;
		int have;
	} rb[4];
	static unsigned next_rb;
	unsigned bpp = pixel_bytes(fmt, type);
	size_t stride = row_stride(w, bpp, pack_alignment, pack_row_length);
	size_t size = stride * (size_t)h;
	size_t rows = BAND_BYTES / stride ? BAND_BYTES / stride : 1;
	int entry = -1;

	LOCK;
	if (size >= DELTA_MIN && size <= ((size_t)32 << 20)) {
		for (int i = 0; i < 4; i++) {
			if (rb[i].shadow && rb[i].dest == pixels && rb[i].x == x && rb[i].y == y && rb[i].w == w && rb[i].h == h && rb[i].fmt == fmt &&
			    rb[i].type == type && rb[i].stride == stride) {
				entry = i;
			}
		}
		if (entry < 0) {
			entry = (int)(next_rb++ & 3);
			free(rb[entry].shadow);
			rb[entry].shadow = malloc(size);
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
			int valid = rb[entry].have && memcmp(pixels, rb[entry].shadow, size) == 0;

			rec_sync(OP_READ_PIXELS_DELTA, A((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fmt, type, (uint32_t)stride, (uint32_t)entry,
			    (uint32_t)valid), 9, NULL, 0);
			uint32_t mode = 0, nb = 0;

			if (reply_n >= 8) {
				memcpy(&mode, replybuf, 4);
				memcpy(&nb, replybuf + 4, 4);
			}
			if (mode == 1 && valid) {
				size_t pos = 8;

				for (uint32_t i = 0; i < nb && pos + 8 <= reply_n; i++) {
					uint32_t row0, nrows;

					memcpy(&row0, replybuf + pos, 4);
					memcpy(&nrows, replybuf + pos + 4, 4);
					pos += 8;
					if (pos + (size_t)nrows * stride > reply_n || (size_t)row0 + nrows > (size_t)h) {
						break;
					}
					memcpy((uint8_t *)pixels + (size_t)row0 * stride, replybuf + pos, (size_t)nrows * stride);
					memcpy(rb[entry].shadow + (size_t)row0 * stride, replybuf + pos, (size_t)nrows * stride);
					pos += (size_t)nrows * stride;
				}
				rb[entry].have = 1;
				UNLOCK;
				return;
			}
			/* everything: the server has kept this readback to compare with the next, so only the bands are read now */
			for (size_t yy = 0; yy < (size_t)h; yy += rows) {
				size_t k = (size_t)h - yy < rows ? (size_t)h - yy : rows;

				rec_sync(OP_READ_PIXELS, A((uint32_t)x, (uint32_t)(y + (GLint)yy), (uint32_t)w, (uint32_t)k, fmt, type, (uint32_t)(stride * k)), 7,
				    NULL, 0);
				memcpy((uint8_t *)pixels + yy * stride, replybuf, reply_n < stride * k ? reply_n : stride * k);
			}
			memcpy(rb[entry].shadow, pixels, size);
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
'''
s = s[:a] + new_read + s[b:]
open(path, 'w', newline='\n').write(s)
print('patched')
