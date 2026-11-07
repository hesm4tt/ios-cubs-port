#!/usr/bin/env python3
"""patch_dirty.py GLSRV.C GL_FWD.C: dirty rows. The server follows which texture each framebuffer draws into and which rows of it
have been drawn into (scissor box of draws and clears, anything else: all of it) since the texture was last read back; a readback of
that texture (OP_READ_PIXELS_DELTA) reads and compares only those rows. Every 64th readback of a place is a full one. The client
keys its copies of readbacks by texture too (it follows the framebuffers the same way)."""
import sys

srv, cli = sys.argv[1:3]
s = open(srv).read()
if 'dirty rows' in s:
    print('already patched')
    sys.exit(0)

track = '''/* ---- dirty rows: which texture a framebuffer draws into, and which rows of it have changed since it was read back */
#define TMAX 4096
#define DFULL_HI (1 << 30)
static GLuint cur_draw_fbo, cur_read_fbo, fbo_color[TMAX], bound2d[32];
static int act_unit, scissor_on, sc[4], dinit;
static int dy0[TMAX], dy1[TMAX];
static unsigned sfull_count[8];

static void
dmark(GLuint t, int y0, int y1)
{
	if (t && t < TMAX) {
		if (y0 < dy0[t]) {
			dy0[t] = y0;
		}
		if (y1 > dy1[t]) {
			dy1[t] = y1;
		}
	}
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
				dmark(t, sc[1], sc[1] + sc[3]);
			} else {
				dfull(t);
			}
		}
		break;
	case OP_BLIT_FRAMEBUFFER:
		dfull(cur_draw_fbo < TMAX ? fbo_color[cur_draw_fbo] : 0);
		break;
	case OP_COPY_TEX_SUB_IMAGE_2D:
	case OP_TEX_IMAGE_2D:
	case OP_TEX_SUB_IMAGE_2D:
	case OP_COMPRESSED_TEX_IMAGE_2D:
	case OP_GEN_MIPMAP:
	case OP_TEX_IMAGE_FROM_CACHE:
		dfull(bound2d[act_unit]);
		break;
	case OP_DELETE_TEXTURES:
		for (uint32_t i = 0; i < a[0]; i++) {
			dfull(a[1 + i]);
		}
		break;
	}
}

'''
marker = 'static void\ndo_op(uint32_t op'
assert marker in s
s = s.replace(marker, track + marker, 1)
s = s.replace('\t(void)nargs;\n\tswitch (op) {', '\t(void)nargs;\n\ttrack(op, a);\n\tswitch (op) {', 1)
assert 'track(op, a);' in s

# the readback case
a0 = s.index('\tcase OP_READ_PIXELS_DELTA: {')
m = s.index('memcpy(sshadow[key], scur, size);', a0)
a1 = s.index('\t}\n', m) + 3
new_case = '''	case OP_READ_PIXELS_DELTA: {
		size_t stride = a[6], rows_total = a[3], size = stride * rows_total;
		uint32_t key = a[7] & 7, valid = a[8], mode = 0, nb = 0;
		GLuint rt = cur_read_fbo < TMAX ? fbo_color[cur_read_fbo] : 0;
		size_t r0 = 0, r1 = rows_total;         /* the rows (from the first asked for) that are read and compared */
		int partial = 0;

		if (size > scur_size) {
			scur = realloc(scur, size);
			scur_size = size;
		}
		if (valid && sshadow[key] && sshadow_size[key] == size && rt && (++sfull_count[key] % 64) != 0) {
			int d0 = dy0[rt] > (int)a[1] ? dy0[rt] : (int)a[1];
			int d1 = dy1[rt] < (int)(a[1] + a[3]) ? dy1[rt] : (int)(a[1] + a[3]);

			partial = 1;
			if (d1 <= d0) {
				r0 = r1 = 0;
			} else {
				r0 = (size_t)d0 - a[1];
				r1 = (size_t)d1 - a[1];
			}
		}
		if (r1 > r0) {
			glReadPixels(a[0], (GLint)(a[1] + r0), a[2], (GLsizei)(r1 - r0), a[4], a[5], scur + r0 * stride);
		}
		if (rt) {
			dy0[rt] = DFULL_HI;
			dy1[rt] = 0;
		}
		reply_len = 8;
		if (valid && sshadow[key] && sshadow_size[key] == size) {
			int overflow = 0;

			for (size_t row0 = r0; row0 < r1; row0 += 32) {
				size_t rows = r1 - row0 < 32 ? r1 - row0 : 32;

				if (memcmp(scur + row0 * stride, sshadow[key] + row0 * stride, rows * stride) != 0) {
					uint32_t hdr[2] = { (uint32_t)row0, (uint32_t)rows };

					if (reply_len + 8 + rows * stride > GL_PAYLOAD_MAX) {
						overflow = 1;
						break;
					}
					memcpy(reply + reply_len, hdr, 8);
					memcpy(reply + reply_len + 8, scur + row0 * stride, rows * stride);
					reply_len += 8 + rows * stride;
					nb++;
				}
			}
			mode = overflow ? 0 : 1;
		}
		if (mode == 0) {
			reply_len = 8;
			nb = 0;
		}
		memcpy(reply, &mode, 4);
		memcpy(reply + 4, &nb, 4);
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
'''
s = s[:a0] + new_case + s[a1:]
open(srv, 'w', newline='\n').write(s)

c = open(cli).read()
# client: follow the read framebuffer and its colour texture
old = 'EXPORT void glBindFramebuffer(GLenum t, GLuint id) { SIMPLE(OP_BIND_FRAMEBUFFER, t, id); }'
assert old in c
c = c.replace(old, '''static GLuint cur_read_fbo, cur_draw_fbo, fbo_color[TEXSHADOW];

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
}''', 1)
old = '''	if (tex < TEXSHADOW && t != 0x8CA8) {          /* (a read framebuffer does not change what is in it) */
		rendered[tex] = 1;
	}
'''
assert old in c
c = c.replace(old, old + '''	if (a == 0x8CE0) {
		GLuint f = t == 0x8CA8 ? cur_read_fbo : cur_draw_fbo;

		if (f < TEXSHADOW) {
			fbo_color[f] = tex;
		}
	}
''', 1)
c = c.replace('\t\tGLenum fmt, type;\n\t\tsize_t stride;\n\t\tuint8_t *shadow;\n\t\tint have;\n\t} rb[4];', '\t\tGLenum fmt, type;\n\t\tsize_t stride;\n\t\tGLuint tex;\n\t\tuint8_t *shadow;\n\t\tint have;\n\t} rb[8];', 1)
c = c.replace('\t\tfor (int i = 0; i < 4; i++) {\n\t\t\tif (rb[i].shadow && rb[i].x == x', '\t\tGLuint rtex = cur_read_fbo < TEXSHADOW ? fbo_color[cur_read_fbo] : 0;\n\n\t\tfor (int i = 0; i < 8; i++) {\n\t\t\tif (rb[i].shadow && rb[i].tex == rtex && rb[i].x == x', 1)
c = c.replace('\t\t\tentry = (int)(next_rb++ & 3);\n\t\t\tfree(rb[entry].shadow);\n\t\t\trb[entry].shadow = malloc(size);\n\t\t\trb[entry].dest = pixels;', '\t\t\tentry = (int)(next_rb++ & 7);\n\t\t\tfree(rb[entry].shadow);\n\t\t\trb[entry].shadow = malloc(size);\n\t\t\trb[entry].tex = rtex;\n\t\t\trb[entry].dest = pixels;', 1)
assert 'rb[entry].tex = rtex' in c and 'rb[i].tex == rtex' in c
open(cli, 'w', newline='\n').write(c)
print('patched')
