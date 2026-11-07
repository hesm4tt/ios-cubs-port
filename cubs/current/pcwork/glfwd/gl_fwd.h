/*
 * OpenGL ES forwarding between iOS's window server and the phone's GPU (Android's EGL/GLES on the PowerVR).
 *
 *   gl_fwd.c   libredwood-gl.dylib's OpenGL ES library (behind src/eagl.c): every gl* call becomes a record in a batch
 *   glsrv.c    the server on Android: executes the batches with EGL + GLES3 and answers the calls that return something
 *
 * Mailbox in the shared ring (16 MB at PA 0xab2000000, the Metal RPC of mtlsrv has 0xB00000..0xE01000):
 *   GL_OFF + 4*i, u32 words: 0 magic "GLF1", 1 request counter, 2 done counter, 3 status (0 = ok), 4 batch length in bytes,
 *   5 result word, 6 reply length in bytes, 7 the first GL error of the batch (glGetError after it), 8.. spare.
 *   Batch and reply at GL_OFF + 0x1000 (up to GL_PAYLOAD_MAX).
 *
 * A batch is records: u32 op, u32 number of argument words, u32 blob length in bytes (blob padded to 4), the argument words
 * (floats as their bits), the blob. Calls that return something (op >= OP_SYNC) end a batch: the client sends what it has and
 * waits. Object names are the server's own (the Gen and Create calls are synchronous), so nothing is translated.
 */
#ifndef GL_FWD_H
#define GL_FWD_H

#define GL_OFF 0xE10000
#define GL_PAYLOAD_OFF (GL_OFF + 0x1000)
#define GL_PAYLOAD_MAX 0x1D0000
#define GL_MAGIC 0x31464c47u /* "GLF1" little endian */

enum gl_op {
	OP_ACTIVE_TEXTURE = 2,
	OP_BIND_TEXTURE,
	OP_BLEND_EQ,
	OP_BLEND_FUNC,
	OP_BLEND_FUNC_SEP,
	OP_CLEAR,
	OP_CLEAR_COLOR,
	OP_CLEAR_STENCIL,
	OP_COPY_TEX_SUB_IMAGE_2D,
	OP_DELETE_TEXTURES,
	OP_DISABLE,
	OP_ENABLE,
	OP_SCISSOR,
	OP_VIEWPORT,
	OP_PIXEL_STORE,
	OP_TEX_PARAM_I,
	OP_TEX_PARAM_F,
	OP_TEX_IMAGE_2D, /* t level ifmt w h border fmt type has_data; blob = pixels */
	OP_TEX_SUB_IMAGE_2D, /* t level xo yo w h fmt type; blob */
	OP_COMPRESSED_TEX_IMAGE_2D, /* t level ifmt w h border size; blob */
	OP_GEN_MIPMAP,
	OP_SHADER_SOURCE, /* id; blob = the source */
	OP_COMPILE_SHADER,
	OP_ATTACH_SHADER,
	OP_LINK_PROGRAM,
	OP_USE_PROGRAM,
	OP_DELETE_PROGRAM,
	OP_UNIFORM1F,
	OP_UNIFORM2F,
	OP_UNIFORM4F,
	OP_UNIFORM1I,
	OP_UNIFORM_VEC, /* loc count comps; blob = floats */
	OP_UNIFORM_MAT, /* loc count dim transpose; blob = floats */
	OP_BIND_ATTRIB_LOC, /* prog index; blob = name */
	OP_BIND_FRAMEBUFFER,
	OP_DELETE_FRAMEBUFFERS,
	OP_FRAMEBUFFER_TEX2D,
	OP_DRAW_ARRAYS, /* mode first count nattr, per attribute: index size type normalized stride bloblen; blob = the arrays */
	OP_DRAW_ELEMENTS, /* mode count type nattr idxlen, per attribute as above; blob = indices, then the arrays */
	OP_FLUSH,
	OP_BLIT_FRAMEBUFFER, /* sx0 sy0 sx1 sy1 dx0 dy0 dx1 dy1 mask filter */
	OP_DELETE_SHADER,
	OP_CACHE_PATCH, /* key row0 rows stride total; blob = the rows: into the server's copy of an image (made at `total` bytes if new) */
	OP_TEX_IMAGE_FROM_CACHE, /* key target level ifmt w h border fmt type: a texture made of that copy by the GPU */
	OP_CACHE_DEFINE, /* key w h ifmt fmt type: the cached image is a texture of the server, empty */
	OP_SYNC = 50,
	OP_CREATE_CTX = OP_SYNC,
	OP_FINISH,
	OP_GEN_TEXTURES,
	OP_GEN_FRAMEBUFFERS,
	OP_CREATE_SHADER,
	OP_CREATE_PROGRAM,
	OP_GET_STRING, /* name -> reply = the string */
	OP_GET_INTEGERV, /* pname -> reply = ints */
	OP_GET_FLOATV,
	OP_GET_UNIFORM_LOCATION, /* prog; blob = name -> result */
	OP_GET_PROGRAMIV,
	OP_GET_SHADERIV,
	OP_GET_SHADER_INFO_LOG,
	OP_GET_PROGRAM_INFO_LOG,
	OP_READ_PIXELS, /* x y w h fmt type -> reply = pixels */
	OP_GET_TEXLEVELPARAM,
	OP_GET_TEXPARAM,
	OP_GET_FB_ATTACH_PARAM,
	OP_GET_ACTIVE_UNIFORM, /* prog index bufsize -> reply: size, type, name */
	OP_GET_ATTACHED_SHADERS, /* prog max -> reply: count, ids */
	OP_IS_TEXTURE,
	OP_IS_PROGRAM,
	OP_GET_UNIFORMFV, /* prog loc -> reply = 16 floats */
	OP_GET_UNIFORMIV,
	OP_IS_ENABLED,
	OP_READ_PIXELS_DELTA, /* x y w h fmt type stride key valid -> reply: u32 mode (1: u32 nbands, per band u32 row0, u32 rows, bytes; 0: send it all) */
};

#endif
