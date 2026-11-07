#!/usr/bin/env python3
"""patch_msaa.py MTLSHIM.M MTLSRV.RS: multisampled colour targets with a resolve texture. Op 7 sixth word = sample count (a 2DMultisample texture),
op 15 has a tenth blend word = raster sample count (vertex-name length now at byte 44, names from 48), and op 12's last trailing section gets a sixth word,
the resolve texture id (the pass resolves into it and does not store the multisampled image)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'MSAA' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


# ---- texture descriptor: type and sample count
s = rep(s, '@synthesize pixelFormat, width, height, usage, mipmapLevelCount;', '@synthesize pixelFormat, width, height, usage, mipmapLevelCount, textureType, sampleCount;')
s = rep(s, '\td.mipmapLevelCount = 1;\n', '\td.mipmapLevelCount = 1;\n\td.sampleCount = 1;\n\td.textureType = MTLTextureType2D;\n')
s = rep(s, 'd.mipmapLevelCount > 1 ? 1u : 0u }, rid = 0;', 'd.mipmapLevelCount > 1 ? 1u : 0u, (uint32_t)(d.sampleCount > 1 ? d.sampleCount : 1) }, rid = 0;')
s = rep(s, '\tuint32_t p[5] = { (uint32_t)d.width', '\tuint32_t p[6] = { (uint32_t)d.width')
s = rep(s, '\tt.mips = d.mipmapLevelCount ? d.mipmapLevelCount : 1;\n', '\tt.mips = d.mipmapLevelCount ? d.mipmapLevelCount : 1;\n\tt.samples = d.sampleCount > 1 ? d.sampleCount : 1; /* MSAA */\n')
s = rep(s, '@property (nonatomic) NSUInteger mips;', '@property (nonatomic) NSUInteger mips, samples;')
s = rep(s, '- (MTLTextureType)textureType { return MTLTextureType2D; }', '- (MTLTextureType)textureType { return self.samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; }\n- (NSUInteger)sampleCount { return self.samples ? self.samples : 1; }')

# ---- pipeline sample count
s = rep(s, '@synthesize vertexFunction, fragmentFunction, depthAttachmentPixelFormat, stencilAttachmentPixelFormat;', '@synthesize vertexFunction, fragmentFunction, depthAttachmentPixelFormat, stencilAttachmentPixelFormat, sampleCount, rasterSampleCount;')
s = rep(s, 'uint32_t blend[9] = {', 'uint32_t blend[10] = {')
s = rep(s, '? 1u : 0u) };\n\n\t[p appendBytes:&lib length:4];', '? 1u : 0u), (uint32_t)(d.sampleCount > 1 ? d.sampleCount : (d.rasterSampleCount > 1 ? d.rasterSampleCount : 1)) };\n\n\t[p appendBytes:&lib length:4];')

# ---- resolve texture on the pass attachment
s = rep(s, '@implementation MTLRenderPassAttachmentDescriptor\n@synthesize texture, loadAction, storeAction;', '@implementation MTLRenderPassAttachmentDescriptor\n@synthesize texture, loadAction, storeAction, resolveTexture, level, slice, depthPlane, resolveLevel, resolveSlice, resolveDepthPlane;')
s = rep(s, '\te.stex = (MSTexture *)desc.stencilAttachment.texture;\n', '\te.stex = (MSTexture *)desc.stencilAttachment.texture;\n\te.rtex = (MSTexture *)c.resolveTexture;\n')
s = rep(s, '@property (nonatomic, retain) MSTexture *depth, *stex;', '@property (nonatomic, retain) MSTexture *depth, *stex, *rtex;')
s = rep(s, '@"stex": self.stex ? (id)self.stex : (id)[NSNull null],', '@"stex": self.stex ? (id)self.stex : (id)[NSNull null], @"rtex": self.rtex ? (id)self.rtex : (id)[NSNull null],')
s = rep(s, '''			uint32_t stl[5] = { [extra[@"stex"] isKindOfClass:[MSTexture class]] ? 1u : 0u, [sr[0] unsignedIntValue], [sr[1] unsignedIntValue],
					    [sr[2] unsignedIntValue], [sr[3] unsignedIntValue] };''', '''			uint32_t stl[6] = { [extra[@"stex"] isKindOfClass:[MSTexture class]] ? 1u : 0u, [sr[0] unsignedIntValue], [sr[1] unsignedIntValue],
					    [sr[2] unsignedIntValue], [sr[3] unsignedIntValue],
					    [extra[@"rtex"] isKindOfClass:[MSTexture class]] ? ((MSTexture *)extra[@"rtex"]).rid : 0u };''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'p.len() >= 20 && u32at(&p, 16) == 1);', 'p.len() >= 20 && u32at(&p, 16) == 1);\n                    let samples = if p.len() >= 24 { u32at(&p, 20) as u64 } else { 1 };')
r = rep(r, '                    t.resource_options = ResourceOptions::private();\n                    texs.insert(next, (device.new_texture(&t)?, tw, th));',
        '''                    if samples > 1 {
                        t.texture_type = TextureType::Type2DMultisample;
                        t.sample_count = samples;
                        t.usage = TextureUsage::RENDER_TARGET;
                    }
                    t.resource_options = ResourceOptions::private();
                    texs.insert(next, (device.new_texture(&t)?, tw, th));''')
r = rep(r, 'let vl = u32at(&p, 40) as usize;\n                    let vname = std::str::from_utf8(&p[44..44 + vl])?;\n                    let fname = std::str::from_utf8(&p[44 + vl..])?;',
        'let vl = u32at(&p, 44) as usize;\n                    let vname = std::str::from_utf8(&p[48..48 + vl])?;\n                    let fname = std::str::from_utf8(&p[48 + vl..])?;')
r = rep(r, '                    d.stencil_format = if u32at(&p, 36) == 2', '                    d.raster_sample_count = u32at(&p, 40).max(1);\n                    d.stencil_format = if u32at(&p, 36) == 2')
r = rep(r, 'let (has_st, sref_f, sref_b, sclear, sload) = {', 'let (has_st, sref_f, sref_b, sclear, sload, resolve_id) = {')
r = rep(r, '''                        if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16))
                        } else {
                            (false, 0, 0, 0, 1)
                        }''', '''                        if p.len() >= o + 24 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20))
                        } else if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), 0)
                        } else {
                            (false, 0, 0, 0, 1, 0)
                        }''')
r = rep(r, '                    a.clear_color = ClearColor::new(f(8), f(12), f(16), f(20));\n                    if dtex_id != 0 {',
        '''                    a.clear_color = ClearColor::new(f(8), f(12), f(16), f(20));
                    if resolve_id != 0 {
                        let (rt, _, _) = texs.get(&resolve_id).ok_or("no such resolve texture")?;
                        a.resolve_texture = Some(rt.clone());
                        a.store_action = StoreAction::MultisampleResolve;
                    }
                    if dtex_id != 0 {''')
open(srv, 'w', newline='\n').write(r)
print('patched')
