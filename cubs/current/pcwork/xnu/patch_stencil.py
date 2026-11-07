#!/usr/bin/env python3
"""patch_stencil.py MTLSHIM.M MTLSRV.RS: stencil testing. Depth32Float_Stencil8 textures (op 7 fourth word = 2) and pipelines (op 15 depth word = 2),
op 16 NEW_DEPTH_STENCIL carries the front and back stencil descriptors (compare, fail op, depth fail op, pass op, read mask, write mask) after its two words,
and op 12 gets a last trailing section after the vertex samplers: has stencil attachment, front ref, back ref, clear stencil, load flag (0 clear, 1 load)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'stencilAttachmentPixelFormat' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new, n=1):
    assert old in text, old[:90]
    return text.replace(old, new, n)


# ---- texture and pipeline format codes
s = rep(s, 'd.pixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u, d.mipmapLevelCount > 1 ? 1u : 0u }',
        '(d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 2u : (d.pixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u)), d.mipmapLevelCount > 1 ? 1u : 0u }')
s = rep(s, 'd.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u };',
        '(d.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float_Stencil8 || d.stencilAttachmentPixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? 2u : (d.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u) };')
s = rep(s, '@synthesize vertexFunction, fragmentFunction, depthAttachmentPixelFormat;', '@synthesize vertexFunction, fragmentFunction, depthAttachmentPixelFormat, stencilAttachmentPixelFormat;')

# ---- descriptor classes
s = rep(s, '@implementation MTLDepthStencilDescriptor\n', '''@implementation MTLStencilDescriptor
@synthesize stencilCompareFunction, stencilFailureOperation, depthFailureOperation, depthStencilPassOperation, readMask, writeMask;
- (instancetype)init
{
	if ((self = [super init])) {
		stencilCompareFunction = MTLCompareFunctionAlways;
		readMask = writeMask = 0xffffffff;
	}
	return self;
}
@end

@implementation MTLRenderPassStencilAttachmentDescriptor
@synthesize clearStencil, stencilResolveFilter;
@end

@implementation MTLDepthStencilDescriptor
''')
s = rep(s, '''	MTLRenderPassDepthAttachmentDescriptor *depth;
}''', '''	MTLRenderPassDepthAttachmentDescriptor *depth;
	MTLRenderPassStencilAttachmentDescriptor *stencil;
}
- (MTLRenderPassStencilAttachmentDescriptor *)stencilAttachment
{
	if (!stencil) {
		stencil = [[MTLRenderPassStencilAttachmentDescriptor alloc] init];
	}
	return stencil;
}''')

# ---- depth stencil state: the stencil descriptors
s = rep(s, '''	uint32_t p[2] = { (uint32_t)d.depthCompareFunction, d.depthWriteEnabled ? 1u : 0u }, rid = 0;
''', '''	uint32_t p[14] = { (uint32_t)d.depthCompareFunction, d.depthWriteEnabled ? 1u : 0u }, rid = 0;
	MTLStencilDescriptor *faces[2] = { d.frontFaceStencil, d.backFaceStencil };

	for (int i = 0; i < 2; i++) {
		uint32_t *q = &p[2 + i * 6];
		MTLStencilDescriptor *f = faces[i];

		q[0] = f ? (uint32_t)f.stencilCompareFunction : (uint32_t)MTLCompareFunctionAlways;
		q[1] = f ? (uint32_t)f.stencilFailureOperation : 0;
		q[2] = f ? (uint32_t)f.depthFailureOperation : 0;
		q[3] = f ? (uint32_t)f.depthStencilPassOperation : 0;
		q[4] = f ? (uint32_t)f.readMask : 0xffffffffu;
		q[5] = f ? (uint32_t)f.writeMask : 0xffffffffu;
	}
''')

# ---- encoder: the stencil attachment and reference values
s = rep(s, '\te.depth = (MSTexture *)desc.depthAttachment.texture;\n',
        '\te.depth = (MSTexture *)desc.depthAttachment.texture;\n\te.stex = (MSTexture *)desc.stencilAttachment.texture;\n\te->clear_stencil = desc.stencilAttachment.clearStencil;\n\te->stencil_load_clears = desc.stencilAttachment.loadAction == MTLLoadActionClear;\n\tif (!e.depth) {\n\t\te.depth = e.stex;\n\t}\n')
s = rep(s, '\tdouble clear_depth;\n}\n@property (nonatomic, retain) MSTexture *depth;',
        '\tdouble clear_depth;\n\tuint32_t clear_stencil, sref_f, sref_b;\n\tBOOL stencil_load_clears;\n}\n@property (nonatomic, retain) MSTexture *depth, *stex;')
s = rep(s, '- (void)setDepthStencilState:(id<MTLDepthStencilState>)state', '''- (void)setStencilReferenceValue:(uint32_t)value
{
	sref_f = sref_b = value;
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	sref_f = front;
	sref_b = back;
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state''')
s = rep(s, '@"vp": self.vpdata', '@"stex": self.stex ? (id)self.stex : (id)[NSNull null], @"sref": @[ @(sref_f), @(sref_b), @(clear_stencil), @((stencil_load_clears && drawn == 0) ? 0u : 1u) ], @"vp": self.vpdata')
s = rep(s, '''			[call appendBytes:&nvs length:4];
			for (NSNumber *k in [vs allKeys]) {
				uint32_t e[2] = { ((MSSampler *)vs[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
''', '''			[call appendBytes:&nvs length:4];
			for (NSNumber *k in [vs allKeys]) {
				uint32_t e[2] = { ((MSSampler *)vs[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
			NSArray *sr = extra[@"sref"];
			uint32_t stl[5] = { [extra[@"stex"] isKindOfClass:[MSTexture class]] ? 1u : 0u, [sr[0] unsignedIntValue], [sr[1] unsignedIntValue],
					    [sr[2] unsignedIntValue], [sr[3] unsignedIntValue] };

			[call appendBytes:stl length:sizeof stl];
''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'let depth = p.len() >= 16 && u32at(&p, 12) == 1;', 'let depth = p.len() >= 16 && u32at(&p, 12) >= 1;\n                    let dsfmt = if p.len() >= 16 && u32at(&p, 12) == 2 { PixelFormat::Depth32FloatStencil8 } else { PixelFormat::Depth32Float };')
r = rep(r, 'if depth { PixelFormat::Depth32Float } else { PixelFormat::RGBA8Unorm }, tw, th,', 'if depth { dsfmt } else { PixelFormat::RGBA8Unorm }, tw, th,')
r = rep(r, 'd.depth_format = if u32at(&p, 36) == 1 { Some(PixelFormat::Depth32Float) } else { None };',
        '''d.depth_format = match u32at(&p, 36) {
                        1 => Some(PixelFormat::Depth32Float),
                        2 => Some(PixelFormat::Depth32FloatStencil8),
                        _ => None,
                    };
                    d.stencil_format = if u32at(&p, 36) == 2 { Some(PixelFormat::Depth32FloatStencil8) } else { None };''')
r = rep(r, '''                    d.depth_write_enabled = u32at(&p, 4) != 0;
''', '''                    d.depth_write_enabled = u32at(&p, 4) != 0;
                    if p.len() >= 56 {
                        let face = |o: usize| -> Result<StencilDescriptor, Box<dyn std::error::Error>> {
                            let mut f = StencilDescriptor::default();
                            f.stencil_compare_function = CompareFunction::try_from(u32at(&p, o))?;
                            f.stencil_failure_operation = StencilOperation::try_from(u32at(&p, o + 4))?;
                            f.depth_failure_operation = StencilOperation::try_from(u32at(&p, o + 8))?;
                            f.depth_stencil_pass_operation = StencilOperation::try_from(u32at(&p, o + 12))?;
                            f.read_mask = u32at(&p, o + 16);
                            f.write_mask = u32at(&p, o + 20);
                            Ok(f)
                        };
                        d.front_face_stencil = face(8)?;
                        d.back_face_stencil = face(32)?;
                    }
''')
r = rep(r, 'use metal2vulkan::metal::state::DepthStencilDescriptor;', 'use metal2vulkan::metal::state::{DepthStencilDescriptor, StencilDescriptor};\nuse metal2vulkan::metal::StencilOperation;')
r = rep(r, '''                    let mut pass = RenderPassDescriptor::new();''', '''                    let (has_st, sref_f, sref_b, sclear, sload) = {
                        let so = 88 + nvb * 12 + nft * 8 + nfb * 12;
                        let nfs = if p.len() >= so + 4 { u32at(&p, so) as usize } else { 0 };
                        let mut o = so + 4 + nfs * 8 + 48;
                        if p.len() >= o + 4 {
                            o += 4 + u32at(&p, o) as usize * 8;
                        }
                        if p.len() >= o + 4 {
                            o += 4 + u32at(&p, o) as usize * 8;
                        }
                        if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16))
                        } else {
                            (false, 0, 0, 0, 1)
                        }
                    };
                    let mut pass = RenderPassDescriptor::new();''')
r = rep(r, '''                        pass.depth_attachment.clear_depth = cdepth;
                    }''', '''                        pass.depth_attachment.clear_depth = cdepth;
                        if has_st {
                            pass.stencil_attachment.texture = Some(dt.clone());
                            pass.stencil_attachment.load_action = if sload == 1 { LoadAction::Load } else { LoadAction::Clear };
                            pass.stencil_attachment.store_action = StoreAction::Store;
                            pass.stencil_attachment.clear_stencil = sclear;
                        }
                    }''')
r = rep(r, '''                    enc.set_cull_mode(CullMode::try_from(cull)?);''', '''                    if has_st {
                        enc.set_stencil_reference_values(sref_f, sref_b);
                    }
                    enc.set_cull_mode(CullMode::try_from(cull)?);''')
open(srv, 'w', newline='\n').write(r)
print('patched')
