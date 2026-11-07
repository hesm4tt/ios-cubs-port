#!/usr/bin/env python3
"""patch_page.py GLSRV.C GL_FWD.C: the changed bands of a readback are paged: the reply is [u32 mode][u32 nb][u32 more] and bands, as many as the
payload takes; the client asks again with arg 9 = 1 until `more` is 0 (the server keeps the frame it read and the list of bands), and
puts the frame into the destination at once at the end. No more fallback to the band-by-band full readback when a frame changes much."""
import sys

srv, cli = sys.argv[1:3]
s = open(srv).read()
if 'pend_n' in s:
    print('already patched')
    sys.exit(0)

decl = '''/* ---- the changed bands of a readback, paged over several replies */
static uint32_t pend_row0[8][260], pend_rows[8][260], pend_n[8], pend_pos[8];

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

'''
marker = 'static void\ndo_op(uint32_t op'
assert marker in s
s = s.replace(marker, decl + marker, 1)

# continuation call
old = '''		uint32_t key = a[7] & 7, valid = a[8], mode = 0, nb = 0;
		GLuint rt = cur_read_fbo < TMAX ? fbo_color[cur_read_fbo] : 0;'''
assert old in s
s = s.replace(old, '''		uint32_t key = a[7] & 7, valid = a[8], mode = 0, nb = 0, more = 0;
		GLuint rt = cur_read_fbo < TMAX ? fbo_color[cur_read_fbo] : 0;

		if (a[9]) {
			uint32_t m1 = 1;

			reply_len = 12;
			nb = emit_pending(key, stride, &more);
			memcpy(reply, &m1, 4);
			memcpy(reply + 4, &nb, 4);
			memcpy(reply + 8, &more, 4);
			break;
		}''', 1)

start = s.index('\t\treply_len = 8;\n\t\tif (valid && sshadow[key] && sshadow_size[key] == size) {\n\t\t\tint overflow = 0;')
end_marker = '\t\tmemcpy(reply + 4, &nb, 4);\n'
end = s.index(end_marker, start) + len(end_marker)
new = '''		reply_len = 12;
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
'''
s = s[:start] + new + s[end:]
open(srv, 'w', newline='\n').write(s)

c = open(cli).read()
# 10 arguments, header of 12 bytes, loop over the pages
old = '''			rec_sync(OP_READ_PIXELS_DELTA, A((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fmt, type, (uint32_t)stride, (uint32_t)entry,
			    (uint32_t)valid), 9, NULL, 0);
			uint32_t mode = 0, nb = 0;

			if (reply_n >= 8) {
				memcpy(&mode, replybuf, 4);
				memcpy(&nb, replybuf + 4, 4);
			}'''
assert old in c
c = c.replace(old, '''			rec_sync(OP_READ_PIXELS_DELTA, A((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, fmt, type, (uint32_t)stride, (uint32_t)entry,
			    (uint32_t)valid, 0u), 10, NULL, 0);
			uint32_t mode = 0, nb = 0, more = 0;

			if (reply_n >= 12) {
				memcpy(&mode, replybuf, 4);
				memcpy(&nb, replybuf + 4, 4);
				memcpy(&more, replybuf + 8, 4);
			}''', 1)
old = '''			if (mode == 1 && valid) {
				size_t pos = 8;

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
				memcpy(pixels, rb[entry].shadow, size);'''
assert old in c
c = c.replace(old, '''			if (mode == 1 && valid) {
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
				memcpy(pixels, rb[entry].shadow, size);''', 1)
open(cli, 'w', newline='\n').write(c)
print('patched')
