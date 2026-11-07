#!/usr/bin/env python3
"""patch_tex.py MTLSHIM.M MTLSRV.RS: sampled textures for the Metal shim. Textures made with ShaderRead usage can be sampled in a fragment
function (setFragmentTexture:atIndex:), filled with replaceRegion:, and several draws of one render pass accumulate (only the first clears).
RPC: op 7 takes an optional third word (1 = sampled), op 12 RENDER_EX
  pso, texture, clear[4] f32, load (0 clear, 1 load), nvb, prim, start, count, nft; nvb x (buffer id, index, offset); nft x (texture id, index),
op 13 TEX_REGION  texture id, x, y, w, h, then w*h*4 bytes (RGBA8)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'RENDER_EX' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new, count=1):
    assert old in text, old[:80]
    return text.replace(old, new, count)


# --- texture creation: say whether the texture is sampled
s = rep(s, '\tuint32_t p[2] = { (uint32_t)d.width, (uint32_t)d.height }, rid = 0;',
        '\tuint32_t p[3] = { (uint32_t)d.width, (uint32_t)d.height, (d.usage & MTLTextureUsageShaderRead) ? 1u : 0u }, rid = 0;')

# --- the encoder
s = rep(s, '''@interface MSRenderEncoder : NSObject <MTLRenderCommandEncoder> {
@public
	float clear[4];
}''', '''@interface MSRenderEncoder : NSObject <MTLRenderCommandEncoder> {
@public
	float clear[4];
	BOOL load_clears;
	unsigned drawn;
}
@property (nonatomic, retain) NSMutableDictionary *ftex;''')
s = rep(s, '\te->clear[3] = (float)c.clearColor.alpha;\n', '\te->clear[3] = (float)c.clearColor.alpha;\n\te->load_clears = c.loadAction == MTLLoadActionClear;\n\te.ftex = [NSMutableDictionary dictionary];\n')
s = rep(s, '''					 @"clear": [NSData dataWithBytes:clear length:16], @"prim": @((uint32_t)primitiveType),
					 @"start": @(vertexStart), @"count": @(vertexCount) }];
}''', '''					 @"clear": [NSData dataWithBytes:clear length:16], @"prim": @((uint32_t)primitiveType),
					 @"start": @(vertexStart), @"count": @(vertexCount), @"load": @((load_clears && drawn == 0) ? 0u : 1u),
					 @"ftex": [self.ftex copy] }];
	drawn++;
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
	if (texture) {
		self.ftex[@(index)] = texture;
	} else {
		[self.ftex removeObjectForKey:@(index)];
	}
}
- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index {}
- (void)setViewport:(MTLViewport)viewport {}
- (void)setScissorRect:(MTLScissorRect)rect {}
- (void)setCullMode:(MTLCullMode)mode {}
- (void)setFrontFacingWinding:(MTLWinding)winding {}''')

# --- commit: op 12
old = '''			uint32_t h0[2] = { rp.rid, tex.rid };
			float *cl = (float *)[d[@"clear"] bytes];
			uint32_t h1[4], h2[4] = { (uint32_t)keys.count, [d[@"prim"] unsignedIntValue], [d[@"start"] unsignedIntValue],
						  [d[@"count"] unsignedIntValue] };
'''
new = '''			uint32_t h0[2] = { rp.rid, tex.rid };
			float *cl = (float *)[d[@"clear"] bytes];
			NSDictionary *ftex = d[@"ftex"];
			NSArray *fkeys = [[ftex allKeys] sortedArrayUsingSelector:@selector(compare:)];
			uint32_t h1[4], h2[6] = { [d[@"load"] unsignedIntValue], (uint32_t)keys.count, [d[@"prim"] unsignedIntValue],
						  [d[@"start"] unsignedIntValue], [d[@"count"] unsignedIntValue], (uint32_t)fkeys.count };
'''
s = rep(s, old, new)
s = rep(s, '''			[call appendBytes:h2 length:16];
			for (NSNumber *k in keys) {
				MSBuffer *b = bound[k][@"buf"];''', '''			[call appendBytes:h2 length:24];
			for (NSNumber *k in keys) {
				MSBuffer *b = bound[k][@"buf"];''')
old = '''			if (rpc_call(9, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			continue;'''
new = '''			for (NSNumber *k in fkeys) {
				uint32_t e[2] = { ((MSTexture *)ftex[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
			if (rpc_call(12, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			continue;'''
s = rep(s, old, new)

# --- replaceRegion on textures
s = rep(s, '''- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
{
	uint32_t id = self.rid, n = 0;''', '''- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow
{
	/* RGBA8 texels, level 0: in bands that fit the RPC's payload */
	NSUInteger w = region.size.width, h = region.size.height, rows = (PAYLOAD_MAX - 64) / (w * 4 ? w * 4 : 1);

	if (rows == 0) {
		return;
	}
	for (NSUInteger y = 0; y < h; y += rows) {
		NSUInteger k = h - y < rows ? h - y : rows;
		uint32_t hdr[5] = { self.rid, (uint32_t)region.origin.x, (uint32_t)(region.origin.y + y), (uint32_t)w, (uint32_t)k };
		NSMutableData *m = [NSMutableData dataWithBytes:hdr length:sizeof hdr];

		for (NSUInteger r = 0; r < k; r++) {
			[m appendBytes:(const uint8_t *)pixelBytes + (y + r) * bytesPerRow length:w * 4];
		}
		(void)rpc_call(13, m.bytes, (uint32_t)m.length, NULL, NULL, 0, NULL);
	}
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)pixelBytes
	  bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage
{
	[self replaceRegion:region mipmapLevel:level withBytes:pixelBytes bytesPerRow:bytesPerRow];
}
- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
{
	uint32_t id = self.rid, n = 0;''')

# --- a sampler state that does nothing (the sampler of the fixtures is in the shader)
s = rep(s, '@implementation MSDevice\n', '''@interface MSSampler : NSObject <MTLSamplerState>
@end
@implementation MSSampler
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSDevice
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	return [[MSSampler alloc] init];
}
''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, '''                    t.usage = TextureUsage::RENDER_TARGET;
                    t.resource_options = ResourceOptions::private();''', '''                    t.usage = if p.len() >= 12 && u32at(&p, 8) & 1 != 0 { TextureUsage::RENDER_TARGET | TextureUsage::SHADER_READ } else { TextureUsage::RENDER_TARGET };
                    t.resource_options = ResourceOptions::private();''')
ops = '''                12 => {
                    // RENDER_EX: pso, texture, clear[4], load, nvb, prim, start, count, nft; nvb x (buffer, index, offset); nft x (texture, index)
                    let st = rpsos.get(&u32at(&p, 0)).ok_or("no such render pipeline")?;
                    let (tex, _, _) = texs.get(&u32at(&p, 4)).ok_or("no such texture")?;
                    let f = |i: usize| f32::from_bits(u32at(&p, i)) as f64;
                    let (load, nvb, prim, start, count, nft) =
                        (u32at(&p, 24), u32at(&p, 28) as usize, u32at(&p, 32), u32at(&p, 36) as u64, u32at(&p, 40), u32at(&p, 44) as usize);
                    let mut pass = RenderPassDescriptor::new();
                    let a = &mut pass.color_attachments[0];
                    a.texture = Some(tex.clone());
                    a.load_action = if load == 1 { LoadAction::Load } else { LoadAction::Clear };
                    a.store_action = StoreAction::Store;
                    a.clear_color = ClearColor::new(f(8), f(12), f(16), f(20));
                    let cb = queue.command_buffer()?;
                    let mut enc = cb.render_command_encoder(&pass)?;
                    enc.set_render_pipeline_state(st)?;
                    for k in 0..nvb {
                        let o = 48 + k * 12;
                        let b = bufs.get(&u32at(&p, o)).ok_or("no such buffer")?;
                        enc.set_vertex_buffer(Some(b), u32at(&p, o + 8) as u64, u32at(&p, o + 4))?;
                    }
                    for k in 0..nft {
                        let o = 48 + nvb * 12 + k * 8;
                        let (t, _, _) = texs.get(&u32at(&p, o)).ok_or("no such fragment texture")?;
                        enc.set_fragment_texture(Some(t), u32at(&p, o + 4))?;
                    }
                    let pt = match prim {
                        0 => PrimitiveType::Point,
                        1 => PrimitiveType::Line,
                        2 => PrimitiveType::LineStrip,
                        4 => PrimitiveType::TriangleStrip,
                        _ => PrimitiveType::Triangle,
                    };
                    enc.draw_primitives(pt, start as u32, count, 1, 0)?;
                    enc.end_encoding()?;
                    cb.commit()?;
                    cb.wait_until_completed_for(Duration::from_secs(20)).map_err(|_| "gpu timeout")?;
                }
                13 => {
                    // TEX_REGION: texture, x, y, w, h, then w*h*4 bytes of RGBA8
                    let (tex, tw, th) = texs.get(&u32at(&p, 0)).ok_or("no such texture")?;
                    let (x, y, w, h) = (u32at(&p, 4) as u64, u32at(&p, 8) as u64, u32at(&p, 12) as u64, u32at(&p, 16) as u64);
                    let need = (w * h * 4) as usize;
                    if x + w > *tw || y + h > *th || p.len() < 20 + need {
                        return Err("texture region does not fit".into());
                    }
                    let sb = device.new_buffer_with_bytes(&p[20..20 + need], ResourceOptions::shared())?;
                    let cb = queue.command_buffer()?;
                    let mut blit = cb.blit_command_encoder()?;
                    blit.copy_from_buffer_to_texture(&sb, 0, w * 4, w * 4 * h, Size::new(w, h, 1), tex, 0, 0, Origin::new(x, y, 0), BlitOption::None)?;
                    blit.end_encoding()?;
                    cb.commit()?;
                    cb.wait_until_completed_for(Duration::from_secs(20)).map_err(|_| "gpu timeout")?;
                }
'''
r = rep(r, '                _ => return Err("unknown op".into()),', ops + '                _ => return Err("unknown op".into()),')
open(srv, 'w', newline='\n').write(r)
print('patched')
