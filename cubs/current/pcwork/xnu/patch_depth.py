#!/usr/bin/env python3
"""patch_depth.py MTLSHIM.M MTLSRV.RS: depth testing, cull mode and winding for the Metal shim. Depth32Float textures (op 7's fourth word = 1), depth stencil states
(op 16 NEW_DEPTH_STENCIL: compare function, depth write), a depth format in the render pipeline (op 15: word 36), and in op 12 RENDER_EX the header is 88 bytes:
  ... idx type (60), depth texture id (64), depth stencil state id (68), clear depth f32 (72), cull mode (76), winding (80), depth load flag (84);
  then the vertex buffers, fragment textures, fragment buffers from 88. Op 14 FREE kind 6: a depth stencil state."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'NEW_DEPTH_STENCIL' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new, all_=False):
    assert old in text, old[:90]
    return text.replace(old, new) if all_ else text.replace(old, new, 1)


# ---- depth textures
s = rep(s, '\tuint32_t p[3] = { (uint32_t)d.width, (uint32_t)d.height, (d.usage & MTLTextureUsageShaderRead) ? 1u : 0u }, rid = 0;',
        '\tuint32_t p[4] = { (uint32_t)d.width, (uint32_t)d.height, (d.usage & MTLTextureUsageShaderRead) ? 1u : 0u,\n\t\t\t  d.pixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u }, rid = 0;')

# ---- classes: depth stencil state, descriptor; the pass's depth attachment; the pipeline's depth format
s = rep(s, '@interface MSSampler : NSObject <MTLSamplerState>', '''@interface MSDepthStencilState : NSObject <MTLDepthStencilState>
@property (nonatomic) uint32_t rid;
@end
@implementation MSDepthStencilState
- (void)dealloc
{
	if (self.rid) {
		uint32_t a[2] = { 6, self.rid };

		(void)rpc_call(14, a, sizeof a, NULL, NULL, 0, NULL);
	}
	[super dealloc];
}
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@interface MSSampler : NSObject <MTLSamplerState>''')
s = rep(s, '@implementation MSDevice\n', '''@implementation MSDevice
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
	uint32_t p[2] = { (uint32_t)d.depthCompareFunction, d.depthWriteEnabled ? 1u : 0u }, rid = 0;

	if (rpc_call(16, p, sizeof p, &rid, NULL, 0, NULL) != 0) {
		return nil;
	}
	MSDepthStencilState *st = [[MSDepthStencilState alloc] init];

	st.rid = rid;
	return st;
}
''')
s = rep(s, '@implementation MTLRenderPassDescriptor {\n\tMTLRenderPassColorAttachmentDescriptorArray *colors;\n}', '''@implementation MTLDepthStencilDescriptor
@synthesize depthCompareFunction, depthWriteEnabled, frontFaceStencil, backFaceStencil, label;
- (instancetype)init
{
	if ((self = [super init])) {
		depthCompareFunction = MTLCompareFunctionAlways;
	}
	return self;
}
@end

@implementation MTLRenderPassDepthAttachmentDescriptor
@synthesize clearDepth, depthResolveFilter;
- (instancetype)init
{
	if ((self = [super init])) {
		clearDepth = 1.0;
	}
	return self;
}
@end

@implementation MTLRenderPassDescriptor {
	MTLRenderPassColorAttachmentDescriptorArray *colors;
	MTLRenderPassDepthAttachmentDescriptor *depth;
}
- (MTLRenderPassDepthAttachmentDescriptor *)depthAttachment
{
	if (!depth) {
		depth = [[MTLRenderPassDepthAttachmentDescriptor alloc] init];
	}
	return depth;
}''')
s = rep(s, '@synthesize vertexFunction, fragmentFunction;', '@synthesize vertexFunction, fragmentFunction, depthAttachmentPixelFormat;')

# ---- pipeline creation: a depth format word between the blend words and the name length
s = rep(s, '''			      (uint32_t)ca.alphaBlendOperation, (uint32_t)ca.writeMask };''',
        '''			      (uint32_t)ca.alphaBlendOperation, (uint32_t)ca.writeMask, d.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u };''')
s = rep(s, '\tuint32_t blend[8] = {', '\tuint32_t blend[9] = {')

# ---- encoder state
s = rep(s, '''	BOOL load_clears;
	unsigned drawn;
}''', '''	BOOL load_clears, depth_load_clears;
	unsigned drawn;
	uint32_t dsid, cull, winding;
	double clear_depth;
}
@property (nonatomic, retain) MSTexture *depth;''')
s = rep(s, '\te->load_clears = c.loadAction == MTLLoadActionClear;\n',
        '\te->load_clears = c.loadAction == MTLLoadActionClear;\n\te.depth = (MSTexture *)desc.depthAttachment.texture;\n\te->clear_depth = desc.depthAttachment.clearDepth;\n\te->depth_load_clears = desc.depthAttachment.loadAction == MTLLoadActionClear;\n')
s = rep(s, '- (void)setCullMode:(MTLCullMode)mode {}\n- (void)setFrontFacingWinding:(MTLWinding)winding {}',
        '''- (void)setCullMode:(MTLCullMode)mode { cull = (uint32_t)mode; }
- (void)setFrontFacingWinding:(MTLWinding)w { winding = (uint32_t)w; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state { dsid = [(MSDepthStencilState *)state rid]; }
- (NSDictionary *)extraState
{
	return @{ @"dtex": self.depth ? (id)self.depth : (id)[NSNull null], @"ds": @(dsid), @"cd": @(clear_depth),
		  @"dload": @((depth_load_clears && drawn == 0) ? 0u : 1u), @"cull": @(cull), @"wind": @(winding) };
}''')
s = rep(s, '@"fbound": [self.fbound copy]', '@"fbound": [self.fbound copy], @"extra": [self extraState]', all_=True)

# ---- commit: the 88-byte header
s = rep(s, '''			MSBuffer *ib = d[@"ibuf"];
			NSMutableData *call = [NSMutableData data];''', '''			MSBuffer *ib = d[@"ibuf"];
			NSDictionary *extra = d[@"extra"];
			MSTexture *dtex = [extra[@"dtex"] isKindOfClass:[MSTexture class]] ? extra[@"dtex"] : nil;
			NSMutableData *call = [NSMutableData data];''')
s = rep(s, '''			memcpy(h1, cl, 16);
			[call appendBytes:h0 length:8];
			[call appendBytes:h1 length:16];
			[call appendBytes:h2 length:40];''', '''			float cd = (float)[extra[@"cd"] doubleValue];
			uint32_t h3[6] = { dtex ? dtex.rid : 0u, [extra[@"ds"] unsignedIntValue], 0, [extra[@"cull"] unsignedIntValue],
					   [extra[@"wind"] unsignedIntValue], [extra[@"dload"] unsignedIntValue] };

			memcpy(&h3[2], &cd, 4);
			memcpy(h1, cl, 16);
			[call appendBytes:h0 length:8];
			[call appendBytes:h1 length:16];
			[call appendBytes:h2 length:40];
			[call appendBytes:h3 length:24];''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'use metal2vulkan::metal::{BlendFactor, BlendOperation, ColorWriteMask, IndexType, PixelFormat, PrimitiveType, TextureUsage};',
        'use metal2vulkan::metal::{BlendFactor, BlendOperation, ColorWriteMask, CompareFunction, CullMode, DepthStencilDescriptor, IndexType, PixelFormat, PrimitiveType, TextureUsage, Winding};')
# texture creation: depth format
r = rep(r, '''                    let mut t = TextureDescriptor::texture_2d(PixelFormat::RGBA8Unorm, tw, th, false);''',
        '''                    let depth = p.len() >= 16 && u32at(&p, 12) == 1;
                    let mut t = TextureDescriptor::texture_2d(if depth { PixelFormat::Depth32Float } else { PixelFormat::RGBA8Unorm }, tw, th, false);''')
# the table of depth stencil states, next to the others
r = rep(r, '    let mut texs: HashMap<u32, (Texture, u64, u64)> = HashMap::new();',
        '    let mut texs: HashMap<u32, (Texture, u64, u64)> = HashMap::new();\n    let mut dss: HashMap<u32, DepthStencilState> = HashMap::new();')
# op 15: depth format word
r = rep(r, '''                    let vl = u32at(&p, 36) as usize;
                    let vname = std::str::from_utf8(&p[40..40 + vl])?;
                    let fname = std::str::from_utf8(&p[40 + vl..])?;''', '''                    let vl = u32at(&p, 40) as usize;
                    let vname = std::str::from_utf8(&p[44..44 + vl])?;
                    let fname = std::str::from_utf8(&p[44 + vl..])?;''')
r = rep(r, '''                    d.color_attachments[0] = Some(ca);
                    rpsos.insert(next, device.new_render_pipeline_state(&d, &PipelineOptions::default())?);''',
        '''                    d.color_attachments[0] = Some(ca);
                    d.depth_format = if u32at(&p, 36) == 1 { Some(PixelFormat::Depth32Float) } else { None };
                    rpsos.insert(next, device.new_render_pipeline_state(&d, &PipelineOptions::default())?);''')
# op 12: header 88, depth attachment, depth stencil state, cull, winding
r = rep(r, '''                    let (nfb, idxbuf, idxoff, idxtype) = (u32at(&p, 48) as usize, u32at(&p, 52), u32at(&p, 56) as u64, u32at(&p, 60));''',
        '''                    let (nfb, idxbuf, idxoff, idxtype) = (u32at(&p, 48) as usize, u32at(&p, 52), u32at(&p, 56) as u64, u32at(&p, 60));
                    let (dtex_id, ds_id, cdepth, cull, wind, dload) =
                        (u32at(&p, 64), u32at(&p, 68), f32::from_bits(u32at(&p, 72)) as f64, u32at(&p, 76), u32at(&p, 80), u32at(&p, 84));''')
r = rep(r, '''                    a.clear_color = ClearColor::new(f(8), f(12), f(16), f(20));
                    let cb = queue.command_buffer()?;
                    let mut enc = cb.render_command_encoder(&pass)?;
                    enc.set_render_pipeline_state(st)?;
                    for k in 0..nvb {
                        let o = 64 + k * 12;''', '''                    a.clear_color = ClearColor::new(f(8), f(12), f(16), f(20));
                    if dtex_id != 0 {
                        let (dt, _, _) = texs.get(&dtex_id).ok_or("no such depth texture")?;
                        pass.depth_attachment.texture = Some(dt.clone());
                        pass.depth_attachment.load_action = if dload == 1 { LoadAction::Load } else { LoadAction::Clear };
                        pass.depth_attachment.store_action = StoreAction::Store;
                        pass.depth_attachment.clear_depth = cdepth;
                    }
                    let cb = queue.command_buffer()?;
                    let mut enc = cb.render_command_encoder(&pass)?;
                    enc.set_render_pipeline_state(st)?;
                    if ds_id != 0 {
                        enc.set_depth_stencil_state(dss.get(&ds_id).ok_or("no such depth stencil state")?)?;
                    }
                    enc.set_cull_mode(CullMode::try_from(cull)?);
                    enc.set_front_facing_winding(Winding::try_from(wind)?);
                    for k in 0..nvb {
                        let o = 88 + k * 12;''')
r = rep(r, 'let o = 64 + nvb * 12 + k * 8;', 'let o = 88 + nvb * 12 + k * 8;')
r = rep(r, 'let o = 64 + nvb * 12 + nft * 8 + k * 12;', 'let o = 88 + nvb * 12 + nft * 8 + k * 12;')
# free kind 6, new op 16
r = rep(r, '''                        5 => {
                            rpsos.remove(&id);
                        }''', '''                        5 => {
                            rpsos.remove(&id);
                        }
                        6 => {
                            dss.remove(&id);
                        }''')
r = rep(r, '                _ => return Err("unknown op".into()),', '''                16 => {
                    // NEW_DEPTH_STENCIL: compare function, depth write
                    let mut d = DepthStencilDescriptor::default();
                    d.depth_compare_function = CompareFunction::try_from(u32at(&p, 0))?;
                    d.depth_write_enabled = u32at(&p, 4) != 0;
                    dss.insert(next, device.new_depth_stencil_state(&d)?);
                    result = next;
                    next += 1;
                }
                _ => return Err("unknown op".into()),''')
open(srv, 'w', newline='\n').write(r)
print('patched')
