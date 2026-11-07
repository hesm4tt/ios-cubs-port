#!/usr/bin/env python3
"""patch_mips.py MTLSHIM.M MTLSRV.RS: mipmapped RGBA8 textures. Op 7 takes a fifth word (1 = mipmapped: a full chain), op 13 TEX_REGION takes the level as a sixth
header word (the pixel data starts at byte 24), op 10 READ_TEXTURE takes an optional level (second word) and returns that level's texels, op 18 GEN_MIPS
(texture id) runs a blit generateMipmaps. The shim has a blit command encoder with generateMipmapsForTexture: (queued in the command buffer like a draw)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'GEN_MIPS' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


# ---- descriptor: the level count
s = rep(s, '''	d.pixelFormat = pf;
	d.width = w;
	d.height = h;
	return d;''', '''	d.pixelFormat = pf;
	d.width = w;
	d.height = h;
	d.mipmapLevelCount = 1;
	if (m) {
		NSUInteger n = 1, side = w > h ? w : h;

		while (side > 1) {
			side >>= 1;
			n++;
		}
		d.mipmapLevelCount = n;
	}
	return d;''')
s = rep(s, '@synthesize pixelFormat, width, height, usage;', '@synthesize pixelFormat, width, height, usage, mipmapLevelCount;')

# ---- texture creation and the texture's own level count
s = rep(s, '''			  d.pixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u }, rid = 0;

	if (rpc_call(7,''', '''			  d.pixelFormat == MTLPixelFormatDepth32Float ? 1u : 0u, d.mipmapLevelCount > 1 ? 1u : 0u }, rid = 0;

	if (rpc_call(7,''')
s = rep(s, '\tuint32_t p[4] = { (uint32_t)d.width, (uint32_t)d.height, (d.usage', '\tuint32_t p[5] = { (uint32_t)d.width, (uint32_t)d.height, (d.usage')
s = rep(s, '\tt.fmt = d.pixelFormat;\n', '\tt.fmt = d.pixelFormat;\n\tt.mips = d.mipmapLevelCount ? d.mipmapLevelCount : 1;\n')
s = rep(s, '@property (nonatomic) MTLPixelFormat fmt;', '@property (nonatomic) MTLPixelFormat fmt;\n@property (nonatomic) NSUInteger mips;')
s = rep(s, '- (NSUInteger)mipmapLevelCount { return 1; }', '- (NSUInteger)mipmapLevelCount { return self.mips ? self.mips : 1; }')

# ---- replaceRegion with a level
s = rep(s, '''	NSUInteger w = region.size.width, h = region.size.height, rows = (PAYLOAD_MAX - 64) / (w * 4 ? w * 4 : 1);''',
        '''	NSUInteger w = region.size.width, h = region.size.height, rows = (PAYLOAD_MAX - 64) / (w * 4 ? w * 4 : 1);
''')
s = rep(s, '''		uint32_t hdr[5] = { self.rid, (uint32_t)region.origin.x, (uint32_t)(region.origin.y + y), (uint32_t)w, (uint32_t)k };''',
        '''		uint32_t hdr[6] = { self.rid, (uint32_t)region.origin.x, (uint32_t)(region.origin.y + y), (uint32_t)w, (uint32_t)k, (uint32_t)level };''')

# ---- getBytes with a level
s = rep(s, '''	uint32_t id = self.rid, n = 0;
	uint8_t *all = malloc(self.w * self.h * 4);

	if (rpc_call(10, &id, 4, NULL, all, (uint32_t)(self.w * self.h * 4), &n) == 0 && n == self.w * self.h * 4) {
		for (NSUInteger y = 0; y < region.size.height; y++) {
			memcpy((uint8_t *)pixelBytes + y * bytesPerRow, all + ((region.origin.y + y) * self.w + region.origin.x) * 4,''',
        '''	uint32_t q[2] = { self.rid, (uint32_t)level }, n = 0;
	NSUInteger lw = self.w >> level ? self.w >> level : 1, lh = self.h >> level ? self.h >> level : 1;
	uint8_t *all = malloc(lw * lh * 4);

	if (rpc_call(10, q, 8, NULL, all, (uint32_t)(lw * lh * 4), &n) == 0 && n == lw * lh * 4) {
		for (NSUInteger y = 0; y < region.size.height; y++) {
			memcpy((uint8_t *)pixelBytes + y * bytesPerRow, all + ((region.origin.y + y) * lw + region.origin.x) * 4,''')

# ---- a blit encoder
s = rep(s, '@implementation MSCommandBuffer\n', '''@interface MSBlitEncoder : NSObject <MTLBlitCommandEncoder>
@property (nonatomic, assign) MSCommandBuffer *cb;
@end
@implementation MSBlitEncoder
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture
{
	[self.cb.dispatches addObject:@{ @"type": @"mips", @"tex": texture }];
}
- (void)endEncoding {}
- (void)setLabel:(NSString *)l {}
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSCommandBuffer
- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	MSBlitEncoder *e = [[MSBlitEncoder alloc] init];

	e.cb = self;
	return e;
}
''')
s = rep(s, '''	for (NSDictionary *d in self.dispatches) {
		if ([d[@"type"] isEqual:@"render"]) {''', '''	for (NSDictionary *d in self.dispatches) {
		if ([d[@"type"] isEqual:@"mips"]) {
			uint32_t id = ((MSTexture *)d[@"tex"]).rid;

			if (rpc_call(18, &id, 4, NULL, NULL, 0, NULL) != 0) { /* GEN_MIPS */
				self.st = MTLCommandBufferStatusError;
			}
			continue;
		}
		if ([d[@"type"] isEqual:@"render"]) {''')
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, 'TextureDescriptor::texture_2d(if depth { PixelFormat::Depth32Float } else { PixelFormat::RGBA8Unorm }, tw, th, false);',
        'TextureDescriptor::texture_2d(if depth { PixelFormat::Depth32Float } else { PixelFormat::RGBA8Unorm }, tw, th, p.len() >= 20 && u32at(&p, 16) == 1);')
r = rep(r, '''                    let (tex, tw, th) = texs.get(&u32at(&p, 0)).ok_or("no such texture")?;
                    let row = tw * 4;
                    let rb = device.new_buffer(row * th, ResourceOptions::shared())?;
                    let cb = queue.command_buffer()?;
                    let mut blit = cb.blit_command_encoder()?;
                    blit.copy_from_texture_to_buffer(tex, 0, 0, Origin::new(0, 0, 0), Size::new(*tw, *th, 1), &rb, 0, row, row * th, BlitOption::None)?;
                    blit.end_encoding()?;
                    cb.commit()?;
                    cb.wait_until_completed_for(Duration::from_secs(20)).map_err(|_| "gpu timeout")?;
                    let src = rb.contents().ok_or("no contents")?;
                    resp = unsafe { std::slice::from_raw_parts(src.as_ptr(), (row * th) as usize) }.to_vec();''',
        '''                    let (tex, tw0, th0) = texs.get(&u32at(&p, 0)).ok_or("no such texture")?;
                    let level = if p.len() >= 8 { u32at(&p, 4) as u64 } else { 0 };
                    let (tw, th) = ((tw0 >> level).max(1), (th0 >> level).max(1));
                    let row = tw * 4;
                    let rb = device.new_buffer(row * th, ResourceOptions::shared())?;
                    let cb = queue.command_buffer()?;
                    let mut blit = cb.blit_command_encoder()?;
                    blit.copy_from_texture_to_buffer(tex, 0, level, Origin::new(0, 0, 0), Size::new(tw, th, 1), &rb, 0, row, row * th, BlitOption::None)?;
                    blit.end_encoding()?;
                    cb.commit()?;
                    cb.wait_until_completed_for(Duration::from_secs(20)).map_err(|_| "gpu timeout")?;
                    let src = rb.contents().ok_or("no contents")?;
                    resp = unsafe { std::slice::from_raw_parts(src.as_ptr(), (row * th) as usize) }.to_vec();''')
r = rep(r, '''                    let (x, y, w, h) = (u32at(&p, 4) as u64, u32at(&p, 8) as u64, u32at(&p, 12) as u64, u32at(&p, 16) as u64);
                    let need = (w * h * 4) as usize;
                    if x + w > *tw || y + h > *th || p.len() < 20 + need {
                        return Err("texture region does not fit".into());
                    }
                    let sb = device.new_buffer_with_bytes(&p[20..20 + need], ResourceOptions::shared())?;''',
        '''                    let (x, y, w, h) = (u32at(&p, 4) as u64, u32at(&p, 8) as u64, u32at(&p, 12) as u64, u32at(&p, 16) as u64);
                    let level = u32at(&p, 20) as u64;
                    let (tw, th) = ((*tw >> level).max(1), (*th >> level).max(1));
                    let need = (w * h * 4) as usize;
                    if x + w > tw || y + h > th || p.len() < 24 + need {
                        return Err("texture region does not fit".into());
                    }
                    let sb = device.new_buffer_with_bytes(&p[24..24 + need], ResourceOptions::shared())?;''')
r = rep(r, 'Size::new(w, h, 1), tex, 0, 0, Origin::new(x, y, 0), BlitOption::None)?;', 'Size::new(w, h, 1), tex, 0, level, Origin::new(x, y, 0), BlitOption::None)?;')
r = rep(r, '                _ => return Err("unknown op".into()),', '''                18 => {
                    // GEN_MIPS: texture id
                    let (tex, _, _) = texs.get(&u32at(&p, 0)).ok_or("no such texture")?;
                    let cb = queue.command_buffer()?;
                    let mut blit = cb.blit_command_encoder()?;
                    blit.generate_mipmaps(tex)?;
                    blit.end_encoding()?;
                    cb.commit()?;
                    cb.wait_until_completed_for(Duration::from_secs(20)).map_err(|_| "gpu timeout")?;
                }
                _ => return Err("unknown op".into()),''')
open(srv, 'w', newline='\n').write(r)
print('patched')
