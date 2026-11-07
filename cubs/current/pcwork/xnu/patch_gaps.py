#!/usr/bin/env python3
"""patch_gaps.py MTLSHIM.M MTLSRV.RS: more of Metal for the shim: objects of the server are freed when the shim's objects are (op 14 FREE), blending
state of the colour attachment (op 15 NEW_RENDER_EX), setVertexBytes/setFragmentBytes/setFragmentBuffer, indexed draws (index buffer, UInt16/UInt32),
instanced variants (one instance). Op 12 RENDER_EX header grows to 64 bytes:
  pso, texture, clear[4], load, nvb, prim, start, count, nft, nfb, index buffer id (0: none), index offset, index type;
  then nvb x (buffer, index, offset), nft x (texture, index), nfb x (buffer, index, offset).
Op 14 FREE: kind (1 buffer, 2 texture, 3 library, 4 compute pipeline, 5 render pipeline), id.
Op 15 NEW_RENDER_EX: library, blend[8] (enabled, srcRGB, dstRGB, rgbOp, srcA, dstA, aOp, writeMask), vertex-name length, vertex name, fragment name."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'NEW_RENDER_EX' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


# ---- freeing
s = rep(s, '@implementation MSBuffer\n', '''@implementation MSBuffer
- (void)dealloc
{
	if (self.rid) {
		uint32_t a[2] = { 1, self.rid };

		(void)rpc_call(14, a, sizeof a, NULL, NULL, 0, NULL);
	}
	free(self.mem);
	[super dealloc];
}
''')
s = rep(s, '@implementation MSTexture\n', '''@implementation MSTexture
- (void)dealloc
{
	if (self.rid) {
		uint32_t a[2] = { 2, self.rid };

		(void)rpc_call(14, a, sizeof a, NULL, NULL, 0, NULL);
	}
	[super dealloc];
}
''')

# ---- blend state of the colour attachment
s = rep(s, '@implementation MTLRenderPipelineColorAttachmentDescriptor\n@synthesize pixelFormat;\n@end',
        '''@implementation MTLRenderPipelineColorAttachmentDescriptor
@synthesize pixelFormat, blendingEnabled, sourceRGBBlendFactor, destinationRGBBlendFactor, rgbBlendOperation, sourceAlphaBlendFactor;
@synthesize destinationAlphaBlendFactor, alphaBlendOperation, writeMask;
- (instancetype)init
{
	if ((self = [super init])) {
		sourceRGBBlendFactor = sourceAlphaBlendFactor = MTLBlendFactorOne;
		destinationRGBBlendFactor = destinationAlphaBlendFactor = MTLBlendFactorZero;
		writeMask = MTLColorWriteMaskAll;
	}
	return self;
}
@end''')
old = '''	uint32_t lib = vf.lib.rid, vl = (uint32_t)vn.length, rid = 0;

	[p appendBytes:&lib length:4];
	[p appendBytes:&vl length:4];
	[p appendData:vn];
	[p appendData:fnn];
	if (rpc_call(8, p.bytes, (uint32_t)p.length, &rid, NULL, 0, NULL) != 0) {'''
new = '''	uint32_t lib = vf.lib.rid, vl = (uint32_t)vn.length, rid = 0;
	MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[0];
	uint32_t blend[8] = { ca.blendingEnabled ? 1u : 0u, (uint32_t)ca.sourceRGBBlendFactor, (uint32_t)ca.destinationRGBBlendFactor,
			      (uint32_t)ca.rgbBlendOperation, (uint32_t)ca.sourceAlphaBlendFactor, (uint32_t)ca.destinationAlphaBlendFactor,
			      (uint32_t)ca.alphaBlendOperation, (uint32_t)ca.writeMask };

	[p appendBytes:&lib length:4];
	[p appendBytes:blend length:sizeof blend];
	[p appendBytes:&vl length:4];
	[p appendData:vn];
	[p appendData:fnn];
	if (rpc_call(15, p.bytes, (uint32_t)p.length, &rid, NULL, 0, NULL) != 0) {'''
s = rep(s, old, new)

# ---- encoder: fragment buffers, bytes, indexed draws
s = rep(s, '@property (nonatomic, retain) NSMutableDictionary *ftex;', '@property (nonatomic, retain) NSMutableDictionary *ftex;\n@property (nonatomic, retain) NSMutableDictionary *fbound;')
s = rep(s, '\te.ftex = [NSMutableDictionary dictionary];\n', '\te.ftex = [NSMutableDictionary dictionary];\n\te.fbound = [NSMutableDictionary dictionary];\n')
s = rep(s, '''					 @"ftex": [self.ftex copy] }];
	drawn++;
}''', '''					 @"ftex": [self.ftex copy], @"fbound": [self.fbound copy] }];
	drawn++;
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
{
	[self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset
{
	[self.cb.dispatches addObject:@{ @"type": @"render", @"pso": self.pso, @"tex": self.target, @"bound": [self.bound copy],
					 @"clear": [NSData dataWithBytes:clear length:16], @"prim": @((uint32_t)primitiveType), @"start": @0,
					 @"count": @(indexCount), @"load": @((load_clears && drawn == 0) ? 0u : 1u), @"ftex": [self.ftex copy],
					 @"fbound": [self.fbound copy], @"ibuf": indexBuffer, @"ioff": @(indexBufferOffset), @"itype": @((uint32_t)indexType) }];
	drawn++;
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount
{
	[self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id<MTLBuffer> b = [MTLCreateSystemDefaultDevice() newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];

	self.bound[@(index)] = @{ @"buf": b, @"off": @0 };
	[b release];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id<MTLBuffer> b = [MTLCreateSystemDefaultDevice() newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];

	self.fbound[@(index)] = @{ @"buf": b, @"off": @0 };
	[b release];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (buffer) {
		self.fbound[@(index)] = @{ @"buf": buffer, @"off": @(offset) };
	} else {
		[self.fbound removeObjectForKey:@(index)];
	}
}''')

# ---- commit: the render branch
a = s.index('\t\tif ([d[@"type"] isEqual:@"render"]) {')
b = s.index('\t\t\tcontinue;\n\t\t}\n', a) + len('\t\t\tcontinue;\n\t\t}\n')
new_branch = '''		if ([d[@"type"] isEqual:@"render"]) {
			MSRenderPipeline *rp = d[@"pso"];
			MSTexture *tex = d[@"tex"];
			NSDictionary *bound = d[@"bound"], *fbound = d[@"fbound"], *ftex = d[@"ftex"];
			NSArray *keys = [[bound allKeys] sortedArrayUsingSelector:@selector(compare:)];
			NSArray *fbkeys = [[fbound allKeys] sortedArrayUsingSelector:@selector(compare:)];
			NSArray *fkeys = [[ftex allKeys] sortedArrayUsingSelector:@selector(compare:)];
			MSBuffer *ib = d[@"ibuf"];
			NSMutableData *call = [NSMutableData data];
			uint32_t h0[2] = { rp.rid, tex.rid };
			float *cl = (float *)[d[@"clear"] bytes];
			uint32_t h1[4], h2[10] = { [d[@"load"] unsignedIntValue], (uint32_t)keys.count, [d[@"prim"] unsignedIntValue],
						   [d[@"start"] unsignedIntValue], [d[@"count"] unsignedIntValue], (uint32_t)fkeys.count,
						   (uint32_t)fbkeys.count, ib ? ib.rid : 0u, [d[@"ioff"] unsignedIntValue], [d[@"itype"] unsignedIntValue] };
			void (^upload)(MSBuffer *) = ^(MSBuffer *b) {
				uint32_t w[2] = { b.rid, 0 };
				NSMutableData *wd = [NSMutableData dataWithBytes:w length:8];

				[wd appendBytes:b.mem length:b.len];
				(void)rpc_call(3, wd.bytes, (uint32_t)wd.length, NULL, NULL, 0, NULL);
			};

			memcpy(h1, cl, 16);
			[call appendBytes:h0 length:8];
			[call appendBytes:h1 length:16];
			[call appendBytes:h2 length:40];
			for (NSNumber *k in keys) {
				MSBuffer *b = bound[k][@"buf"];

				upload(b);
				uint32_t e[3] = { b.rid, (uint32_t)k.unsignedIntValue, (uint32_t)[bound[k][@"off"] unsignedIntValue] };

				[call appendBytes:e length:sizeof e];
			}
			for (NSNumber *k in fkeys) {
				uint32_t e[2] = { ((MSTexture *)ftex[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
			for (NSNumber *k in fbkeys) {
				MSBuffer *b = fbound[k][@"buf"];

				upload(b);
				uint32_t e[3] = { b.rid, (uint32_t)k.unsignedIntValue, (uint32_t)[fbound[k][@"off"] unsignedIntValue] };

				[call appendBytes:e length:sizeof e];
			}
			if (ib) {
				upload(ib);
			}
			if (rpc_call(12, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			continue;
		}
'''
s = s[:a] + new_branch + s[b:]
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureUsage};',
        'use metal2vulkan::metal::{BlendFactor, BlendOperation, ColorWriteMask, IndexType, PixelFormat, PrimitiveType, TextureUsage};')
# op 12: header of 64 bytes, fragment buffers, indexed draws
r = rep(r, '''                    let (load, nvb, prim, start, count, nft) =
                        (u32at(&p, 24), u32at(&p, 28) as usize, u32at(&p, 32), u32at(&p, 36) as u64, u32at(&p, 40), u32at(&p, 44) as usize);''',
        '''                    let (load, nvb, prim, start, count, nft) =
                        (u32at(&p, 24), u32at(&p, 28) as usize, u32at(&p, 32), u32at(&p, 36) as u64, u32at(&p, 40), u32at(&p, 44) as usize);
                    let (nfb, idxbuf, idxoff, idxtype) = (u32at(&p, 48) as usize, u32at(&p, 52), u32at(&p, 56) as u64, u32at(&p, 60));''')
r = rep(r, '''                    for k in 0..nvb {
                        let o = 48 + k * 12;
                        let b = bufs.get(&u32at(&p, o)).ok_or("no such buffer")?;
                        enc.set_vertex_buffer(Some(b), u32at(&p, o + 8) as u64, u32at(&p, o + 4))?;
                    }
                    for k in 0..nft {
                        let o = 48 + nvb * 12 + k * 8;
                        let (t, _, _) = texs.get(&u32at(&p, o)).ok_or("no such fragment texture")?;
                        enc.set_fragment_texture(Some(t), u32at(&p, o + 4))?;
                    }''', '''                    for k in 0..nvb {
                        let o = 64 + k * 12;
                        let b = bufs.get(&u32at(&p, o)).ok_or("no such buffer")?;
                        enc.set_vertex_buffer(Some(b), u32at(&p, o + 8) as u64, u32at(&p, o + 4))?;
                    }
                    for k in 0..nft {
                        let o = 64 + nvb * 12 + k * 8;
                        let (t, _, _) = texs.get(&u32at(&p, o)).ok_or("no such fragment texture")?;
                        enc.set_fragment_texture(Some(t), u32at(&p, o + 4))?;
                    }
                    for k in 0..nfb {
                        let o = 64 + nvb * 12 + nft * 8 + k * 12;
                        let b = bufs.get(&u32at(&p, o)).ok_or("no such fragment buffer")?;
                        enc.set_fragment_buffer(Some(b), u32at(&p, o + 8) as u64, u32at(&p, o + 4))?;
                    }''')
# the draw of op 12 only (the first draw_primitives after 'no such fragment buffer'): replace the one in the op 12 branch
i12 = r.index('12 => {')
j = r.index('enc.draw_primitives(pt, start as u32, count, 1, 0)?;', i12)
r = r[:j] + '''if idxbuf != 0 {
                        let ib = bufs.get(&idxbuf).ok_or("no such index buffer")?;
                        let it = if idxtype == 1 { IndexType::UInt32 } else { IndexType::UInt16 };
                        enc.draw_indexed_primitives(pt, count, it, ib, idxoff, 1, 0, 0)?;
                    } else {
                        enc.draw_primitives(pt, start as u32, count, 1, 0)?;
                    }''' + r[j + len('enc.draw_primitives(pt, start as u32, count, 1, 0)?;'):]

more = '''                14 => {
                    // FREE: kind (1 buffer, 2 texture, 3 library, 4 compute pipeline, 5 render pipeline), id
                    let id = u32at(&p, 4);
                    match u32at(&p, 0) {
                        1 => {
                            bufs.remove(&id);
                        }
                        2 => {
                            texs.remove(&id);
                        }
                        3 => {
                            libs.remove(&id);
                        }
                        4 => {
                            psos.remove(&id);
                        }
                        5 => {
                            rpsos.remove(&id);
                        }
                        _ => {}
                    }
                }
                15 => {
                    // NEW_RENDER_EX: library, blend[8], vertex-name length, vertex name, fragment name
                    let lib = libs.get(&u32at(&p, 0)).ok_or("no such library")?;
                    let vl = u32at(&p, 36) as usize;
                    let vname = std::str::from_utf8(&p[40..40 + vl])?;
                    let fname = std::str::from_utf8(&p[40 + vl..])?;
                    let vf = lib.new_function_with_name(vname)?;
                    let ff = lib.new_function_with_name(fname)?;
                    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vf.function_descriptor()));
                    d.fragment = Some(StageSource::Function(ff.function_descriptor()));
                    let mut ca = ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm);
                    ca.blending_enabled = u32at(&p, 4) != 0;
                    ca.source_rgb_blend_factor = BlendFactor::try_from(u32at(&p, 8))?;
                    ca.destination_rgb_blend_factor = BlendFactor::try_from(u32at(&p, 12))?;
                    ca.rgb_blend_operation = BlendOperation::try_from(u32at(&p, 16))?;
                    ca.source_alpha_blend_factor = BlendFactor::try_from(u32at(&p, 20))?;
                    ca.destination_alpha_blend_factor = BlendFactor::try_from(u32at(&p, 24))?;
                    ca.alpha_blend_operation = BlendOperation::try_from(u32at(&p, 28))?;
                    ca.write_mask = ColorWriteMask::try_from(u32at(&p, 32))?;
                    d.color_attachments[0] = Some(ca);
                    rpsos.insert(next, device.new_render_pipeline_state(&d, &PipelineOptions::default())?);
                    result = next;
                    next += 1;
                }
'''
r = rep(r, '                _ => return Err("unknown op".into()),', more + '                _ => return Err("unknown op".into()),')
open(srv, 'w', newline='\n').write(r)
print('patched')
