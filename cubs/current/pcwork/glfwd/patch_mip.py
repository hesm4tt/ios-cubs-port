#!/usr/bin/env python3
"""patch_mip.py GLSRV.C: skip the readback of a frame that has not changed. Before a readback of a whole texture (OP_READ_PIXELS_DELTA)
the server makes the texture's mipmaps, reads a small level (1/8 by 1/8: every pixel is in the average of a block) and compares it
with the one of the previous readback of the same place: equal, and the client keeps the copy it has (mode 1, no bands). A change
smaller than the rounding of a block average goes unseen until the full readback every 64th time."""
import sys

path = sys.argv[1]
s = open(path).read()
if 'mip_read' in s:
    print('already patched')
    sys.exit(0)

helper = '''/* ---- a small mipmap level of a texture, to tell whether it has changed */
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

'''
marker = 'static void\ndo_op(uint32_t op'
assert marker in s
s = s.replace(marker, helper + marker, 1)

old = '''		if (valid && sshadow[key] && sshadow_size[key] == size && rt && (++sfull_count[key] % 64) != 0) {'''
assert old in s
s = s.replace(old, '''		size_t msz = 0;
		int have_mip = rt && a[0] == 0 && a[1] == 0 && mip_read(rt, a[2], a[3], &msz);

		if (valid && sshadow[key] && sshadow_size[key] == size && rt && (++sfull_count[key] % 64) != 0) {''', 1)

old = '''			partial = 1;
			if (d1 <= d0) {
				r0 = r1 = 0;
			} else {
				r0 = (size_t)d0 - a[1];
				r1 = (size_t)d1 - a[1];
			}
		}'''
assert old in s
s = s.replace(old, old[:-4] + '''			if (have_mip && smip[key] && smip_size[key] == msz && memcmp(smip[key], mipscratch, msz) == 0) {
				r0 = r1 = 0;                    /* unchanged: nothing is read, the client's copy stands */
				op_count[113]++;
			}
		}''', 1)

old = '''		reply_len = 8;
		if (valid && sshadow[key] && sshadow_size[key] == size) {
			int overflow = 0;
'''
assert old in s
s = s.replace(old, '''		if (have_mip) {
			if (smip_size[key] != msz) {
				smip[key] = realloc(smip[key], msz);
				smip_size[key] = msz;
			}
			memcpy(smip[key], mipscratch, msz);
		}
''' + old, 1)
s = s.replace('(i >= 105 && i <= 112 && op_count[i])', '(i >= 105 && i <= 113 && op_count[i])', 1)
open(path, 'w', newline='\n').write(s)
print('patched')
