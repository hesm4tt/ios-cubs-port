#!/usr/bin/env python3
"""patch_state.py GL_FWD.C: answer glGetIntegerv / glGetFramebufferAttachmentParameteriv from state the client already follows (pixel store, bound
texture and framebuffers, program, viewport, scissor, the colour attachment of a framebuffer) instead of a round trip each; count the pnames of
the glGetFloatv calls that are still sent."""
import sys

path = sys.argv[1]
s = open(path).read()
if 'cur_program' in s:
    print('already patched')
    sys.exit(0)


def rep(old, new):
    global s
    assert old in s, old[:70]
    s = s.replace(old, new, 1)


rep('EXPORT void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { SIMPLE(OP_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); }\n'
    'EXPORT void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { SIMPLE(OP_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); }',
    '''static GLint vp_state[4], sc_state[4];
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
}''')
rep('EXPORT void glUseProgram(GLuint p) { SIMPLE(OP_USE_PROGRAM, p); }', 'EXPORT void glUseProgram(GLuint p) { cur_program = p; SIMPLE(OP_USE_PROGRAM, p); }')

rep('''	LOCK;
	if (static_pname(pname) && pname < 0x10000 && have[pname]) {''', '''	switch (pname) {
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
	if (static_pname(pname) && pname < 0x10000 && have[pname]) {''')

rep('''glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum pname, GLint *v)
{
	LOCK;''', '''glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum pname, GLint *v)
{
	if (a == 0x8CE0 && (pname == 0x8CD1 || pname == 0x8CD0)) {
		GLuint f = t == 0x8CA8 ? cur_read_fbo : cur_draw_fbo;

		if (f != 0 && f < TEXSHADOW && fbo_color[f] != 0) {
			*v = pname == 0x8CD1 ? (GLint)fbo_color[f] : 0x1702;
			return;
		}
	}
	LOCK;''')

rep('''glGetFloatv(GLenum pname, GLfloat *v)
{
	LOCK;''', '''glGetFloatv(GLenum pname, GLfloat *v)
{
	static GLenum fl_p[8];
	static unsigned long fl_c[8];

	LOCK;
	for (int i = 0; i < 8; i++) {
		if (fl_c[i] == 0 || fl_p[i] == pname) {
			fl_p[i] = pname;
			if (++fl_c[i] % 5000 == 0) {
				say("redwood-gl: forward: glGetFloatv %#x asked %lu times\\n", pname, fl_c[i]);
			}
			break;
		}
	}''')
open(path, 'w', newline='\n').write(s)
print('patched')
