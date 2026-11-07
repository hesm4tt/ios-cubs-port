#!/usr/bin/env python3
"""patch_occ.py MTLSHIM.M MTLSRV.RS: occlusion queries. The pass descriptor's visibilityResultBuffer and the encoder's setVisibilityResultMode:offset: reach the
GPU: op 12 gets three more words after the instance words: visibility buffer id, mode (0 disabled, 1 boolean, 2 counting), offset. The server runs the draw as
its own pass with the buffer attached; the shim then reads the 8-byte result back (op 4) and combines it into the buffer's host copy: the first draw after a
setVisibilityResultMode call stores it, later draws under the same call add to it (a result spans the draws of its region)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'pend_vis' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new, n=1):
    assert old in text, old[:90]
    return text.replace(old, new, n)


# ---- descriptor property
s = rep(s, '@implementation MTLRenderPassDescriptor {', '@implementation MTLRenderPassDescriptor {\n\tid<MTLBuffer> visBuf;')
s = rep(s, '- (MTLRenderPassStencilAttachmentDescriptor *)stencilAttachment', '''- (id<MTLBuffer>)visibilityResultBuffer { return visBuf; }
- (void)setVisibilityResultBuffer:(id<MTLBuffer>)b
{
	[b retain];
	[visBuf release];
	visBuf = b;
}
- (MTLRenderPassStencilAttachmentDescriptor *)stencilAttachment''')

# ---- encoder state
s = rep(s, '\tNSDictionary *pend_ind;\n', '\tNSDictionary *pend_ind;\n\tuint32_t vis_mode, vis_off, vis_region;\n\tBOOL vis_first;\n')
s = rep(s, '@property (nonatomic, retain) MSTexture *depth, *stex, *rtex;', '@property (nonatomic, retain) MSTexture *depth, *stex, *rtex;\n@property (nonatomic, retain) id<MTLBuffer> visbuf;')
s = rep(s, '\te.rtex = (MSTexture *)c.resolveTexture;\n', '\te.rtex = (MSTexture *)c.resolveTexture;\n\te.visbuf = desc.visibilityResultBuffer;\n')
s = rep(s, '- (void)setStencilReferenceValue:(uint32_t)value', '''- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
	vis_mode = (uint32_t)mode;
	vis_off = (uint32_t)offset;
	vis_first = YES; /* pend_vis: the next draw opens a new result */
}
- (void)setStencilReferenceValue:(uint32_t)value''')
# every base draw records the visibility state
s = rep(s, '''@"ind": pend_ind ? (id)pend_ind : (id)[NSNull null] }];
	pend_ind = nil;''', '''@"ind": pend_ind ? (id)pend_ind : (id)[NSNull null],
					 @"vis": (vis_mode && self.visbuf) ? (id)@[ self.visbuf, @(vis_mode), @(vis_off), @(vis_first) ] : (id)[NSNull null] }];
	vis_first = NO;
	pend_ind = nil;''', 2)

# ---- commit: three words, then read the result back after the call
s = rep(s, '''			[call appendBytes:di length:sizeof di];
''', '''			[call appendBytes:di length:sizeof di];
			NSArray *vis = [d[@"vis"] isKindOfClass:[NSArray class]] ? d[@"vis"] : nil;
			uint32_t vw[3] = { vis ? ((MSBuffer *)vis[0]).rid : 0u, vis ? [vis[1] unsignedIntValue] : 0u, vis ? [vis[2] unsignedIntValue] : 0u };

			[call appendBytes:vw length:sizeof vw];
''')
s = rep(s, '''			if (rpc_call(12, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			continue;''', '''			if (rpc_call(12, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			if (vis) {
				MSBuffer *vb = vis[0];
				uint32_t r[3] = { vb.rid, [vis[2] unsignedIntValue], 8 }, n = 0;
				uint64_t got = 0;

				if (rpc_call(4, r, sizeof r, NULL, &got, 8, &n) == 0 && n == 8) {
					uint64_t *dst = (uint64_t *)((uint8_t *)vb.mem + [vis[2] unsignedIntValue]);

					*dst = [vis[3] boolValue] ? got : *dst + got;
					if ([vis[1] unsignedIntValue] == 1) {
						*dst = *dst ? 1 : 0;
					}
				}
			}
			continue;''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'let (has_st, sref_f, sref_b, sclear, sload, resolve_id, ninst, binst, bvert) = {', 'let (has_st, sref_f, sref_b, sclear, sload, resolve_id, ninst, binst, bvert, vis_id, vis_mode, vis_off) = {')
r = rep(r, '''                        if p.len() >= o + 36 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20),
                             u32at(&p, o + 24).max(1), u32at(&p, o + 28), u32at(&p, o + 32) as i32)
                        } else if p.len() >= o + 24 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20), 1, 0, 0)
                        } else if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), 0, 1, 0, 0)
                        } else {
                            (false, 0, 0, 0, 1, 0, 1, 0, 0)
                        }''', '''                        if p.len() >= o + 48 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20),
                             u32at(&p, o + 24).max(1), u32at(&p, o + 28), u32at(&p, o + 32) as i32, u32at(&p, o + 36), u32at(&p, o + 40), u32at(&p, o + 44) as u64)
                        } else if p.len() >= o + 36 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20),
                             u32at(&p, o + 24).max(1), u32at(&p, o + 28), u32at(&p, o + 32) as i32, 0, 0, 0)
                        } else if p.len() >= o + 24 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20), 1, 0, 0, 0, 0, 0)
                        } else if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), 0, 1, 0, 0, 0, 0, 0)
                        } else {
                            (false, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0)
                        }''')
r = rep(r, '                    let cb = queue.command_buffer()?;\n                    let mut enc = cb.render_command_encoder(&pass)?;\n                    enc.set_render_pipeline_state(st)?;\n                    if ds_id != 0 {',
        '''                    if vis_id != 0 && vis_mode != 0 {
                        pass.visibility_result_buffer = Some(bufs.get(&vis_id).ok_or("no such visibility buffer")?.clone());
                    }
                    let cb = queue.command_buffer()?;
                    let mut enc = cb.render_command_encoder(&pass)?;
                    enc.set_render_pipeline_state(st)?;
                    if vis_id != 0 && vis_mode != 0 {
                        enc.set_visibility_result_mode(if vis_mode == 2 { VisibilityResultMode::Counting } else { VisibilityResultMode::Boolean }, vis_off)?;
                    }
                    if ds_id != 0 {''')
r = rep(r, 'use metal2vulkan::metal::StencilOperation;', 'use metal2vulkan::metal::{StencilOperation, VisibilityResultMode};')
open(srv, 'w', newline='\n').write(r)
print('patched')
