#!/usr/bin/env python3
"""patch_tex3d.py MTLSHIM.M MTLSRV.RS: 2D array, cube, cube array and 3D textures (RGBA8/BGRA8) with uploads and readbacks per slice or depth plane.
Op 7 words 6..8 = texture type, depth, array length (the server makes the descriptor with them; a cube's six faces are slices 0..5). Op 13 TEX_REGION's header is
8 words (texture, x, y, w, h, level, slice, z) and the texels start at byte 32. Op 10 READ_TEXTURE with 24 bytes: texture, level, y0, rows (0 = all), slice, z.
The server keeps each texture's type to pick slice (array and cube types) or z origin (3D)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'cubeDescriptor' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


# ---- descriptor: depth, array length, cube factory
s = rep(s, '@synthesize pixelFormat, width, height, usage, mipmapLevelCount, textureType, sampleCount;', '@synthesize pixelFormat, width, height, usage, mipmapLevelCount, textureType, sampleCount, depth, arrayLength;')
s = rep(s, '\td.sampleCount = 1;\n\td.textureType = MTLTextureType2D;\n', '\td.sampleCount = 1;\n\td.depth = 1;\n\td.arrayLength = 1;\n\td.textureType = MTLTextureType2D;\n')
s = rep(s, '+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:', '''+ (MTLTextureDescriptor *)textureCubeDescriptorWithPixelFormat:(MTLPixelFormat)pf size:(NSUInteger)size mipmapped:(BOOL)m
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:size height:size mipmapped:m];

	d.textureType = MTLTextureTypeCube; /* cubeDescriptor */
	return d;
}
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:''')

# ---- texture creation
s = rep(s, '(uint32_t)(d.sampleCount > 1 ? d.sampleCount : 1) }, rid = 0;', '(uint32_t)(d.sampleCount > 1 ? d.sampleCount : 1),\n\t\t\t  (uint32_t)d.textureType, (uint32_t)(d.depth ? d.depth : 1), (uint32_t)(d.arrayLength ? d.arrayLength : 1) }, rid = 0;')
s = rep(s, '\tuint32_t p[6] = { (uint32_t)d.width', '\tuint32_t p[9] = { (uint32_t)d.width')
s = rep(s, '\tt.samples = d.sampleCount > 1 ? d.sampleCount : 1; /* MSAA */\n', '\tt.samples = d.sampleCount > 1 ? d.sampleCount : 1; /* MSAA */\n\tt.ttype = d.textureType;\n\tt.dep = d.depth ? d.depth : 1;\n\tt.alen = d.arrayLength ? d.arrayLength : 1;\n')
s = rep(s, '@property (nonatomic) NSUInteger mips, samples;', '@property (nonatomic) NSUInteger mips, samples, ttype, dep, alen;')
s = rep(s, '- (NSUInteger)depth { return 1; }', '- (NSUInteger)depth { return self.dep ? self.dep : 1; }\n- (NSUInteger)arrayLength { return self.alen ? self.alen : 1; }')
s = rep(s, '- (MTLTextureType)textureType { return self.samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; }',
        '- (MTLTextureType)textureType { return self.samples > 1 ? MTLTextureType2DMultisample : (self.ttype ? (MTLTextureType)self.ttype : MTLTextureType2D); }')

# ---- reading one slice / depth plane
s = rep(s, '''static int
read_level(uint32_t rid, uint32_t level, NSUInteger lw, NSUInteger lh, uint8_t *dst)
{
	NSUInteger rows = (PAYLOAD_MAX - 64) / (lw * 4), y = 0;

	if (rows == 0) {
		return -1;
	}
	if (lw * lh * 4 <= PAYLOAD_MAX - 64) {
		uint32_t q[2] = { rid, level }, n = 0;

		return rpc_call(10, q, 8, NULL, dst, (uint32_t)(lw * lh * 4), &n) == 0 && n == lw * lh * 4 ? 0 : -1;
	}
	while (y < lh) {
		NSUInteger k = lh - y < rows ? lh - y : rows;
		uint32_t q[4] = { rid, level, (uint32_t)y, (uint32_t)k }, n = 0;

		if (rpc_call(10, q, 16, NULL, dst + y * lw * 4, (uint32_t)(k * lw * 4), &n) != 0 || n != k * lw * 4) {
			return -1;
		}
		y += k;
	}
	return 0;''', '''static int
read_level_slice(uint32_t rid, uint32_t level, NSUInteger lw, NSUInteger lh, uint32_t slice, uint32_t zplane, uint8_t *dst)
{
	NSUInteger rows = (PAYLOAD_MAX - 64) / (lw * 4), y = 0;

	if (rows == 0) {
		return -1;
	}
	if (lw * lh * 4 <= PAYLOAD_MAX - 64) {
		uint32_t q[6] = { rid, level, 0, 0, slice, zplane }, n = 0;

		return rpc_call(10, q, 24, NULL, dst, (uint32_t)(lw * lh * 4), &n) == 0 && n == lw * lh * 4 ? 0 : -1;
	}
	while (y < lh) {
		NSUInteger k = lh - y < rows ? lh - y : rows;
		uint32_t q[6] = { rid, level, (uint32_t)y, (uint32_t)k, slice, zplane }, n = 0;

		if (rpc_call(10, q, 24, NULL, dst + y * lw * 4, (uint32_t)(k * lw * 4), &n) != 0 || n != k * lw * 4) {
			return -1;
		}
		y += k;
	}
	return 0;''')
# a 2D wrapper with the old name for the other callers (present, getBytes)
s = rep(s, '@implementation MSTexture\n', '''static int
read_level(uint32_t rid, uint32_t level, NSUInteger lw, NSUInteger lh, uint8_t *dst)
{
	return read_level_slice(rid, level, lw, lh, 0, 0, dst);
}

@implementation MSTexture
''')

# ---- replaceRegion core (slice, depth planes)
a = s.index('- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow\n{')
b = s.index('- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level\n{')
c = s.index('- (NSString *)label { return nil; }', b)
new_rw = '''- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)pixelBytes
	  bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage
{
	/* RGBA8/BGRA8 texels of one array slice or cube face, or of the depth planes of a 3D texture, in bands that fit the RPC's payload */
	NSUInteger w = region.size.width, h = region.size.height, planes = region.size.depth ? region.size.depth : 1, rows = (PAYLOAD_MAX - 96) / (w * 4 ? w * 4 : 1);

	if (rows == 0) {
		return;
	}
	for (NSUInteger pl = 0; pl < planes; pl++) {
		for (NSUInteger y = 0; y < h; y += rows) {
			NSUInteger k = h - y < rows ? h - y : rows;
			uint32_t hdr[8] = { self.rid, (uint32_t)region.origin.x, (uint32_t)(region.origin.y + y), (uint32_t)w, (uint32_t)k, (uint32_t)level, (uint32_t)slice,
					    (uint32_t)(region.origin.z + pl) };
			NSMutableData *m = [NSMutableData dataWithBytes:hdr length:sizeof hdr];

			for (NSUInteger r = 0; r < k; r++) {
				[m appendBytes:(const uint8_t *)pixelBytes + pl * bytesPerImage + (y + r) * bytesPerRow length:w * 4];
				if (self.fmt == MTLPixelFormatBGRA8Unorm) {
					uint8_t *q = (uint8_t *)m.mutableBytes + m.length - w * 4;

					for (NSUInteger i = 0; i < w; i++) {
						uint8_t t = q[i * 4];

						q[i * 4] = q[i * 4 + 2];
						q[i * 4 + 2] = t;
					}
				}
			}
			(void)rpc_call(13, m.bytes, (uint32_t)m.length, NULL, NULL, 0, NULL);
		}
	}
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:pixelBytes bytesPerRow:bytesPerRow bytesPerImage:0];
}
- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	NSUInteger lw = self.w >> level ? self.w >> level : 1, lh = self.h >> level ? self.h >> level : 1, planes = region.size.depth ? region.size.depth : 1;
	uint8_t *all = malloc(lw * lh * 4);

	for (NSUInteger pl = 0; pl < planes; pl++) {
		if (read_level_slice(self.rid, (uint32_t)level, lw, lh, (uint32_t)slice, (uint32_t)(region.origin.z + pl), all) != 0) {
			break;
		}
		for (NSUInteger y = 0; y < region.size.height; y++) {
			uint8_t *q = (uint8_t *)pixelBytes + pl * bytesPerImage + y * bytesPerRow;

			memcpy(q, all + ((region.origin.y + y) * lw + region.origin.x) * 4, region.size.width * 4);
			if (self.fmt == MTLPixelFormatBGRA8Unorm) {
				for (NSUInteger i = 0; i < region.size.width; i++) {
					uint8_t t = q[i * 4];

					q[i * 4] = q[i * 4 + 2];
					q[i * 4 + 2] = t;
				}
			}
		}
	}
	free(all);
}
- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
{
	[self getBytes:pixelBytes bytesPerRow:bytesPerRow bytesPerImage:0 fromRegion:region mipmapLevel:level slice:0];
}
'''
s = s[:a] + new_rw + s[c:]
open(shim, 'w', newline='\n').write(s)

# ---------------------------------------------------------------------------------------------------- the server
r = open(srv).read()
r = rep(r, '    let mut samplers: HashMap<u32, SamplerState> = HashMap::new();', '    let mut samplers: HashMap<u32, SamplerState> = HashMap::new();\n    let mut ttypes: HashMap<u32, u32> = HashMap::new();')
# op 7
r = rep(r, '''                    if samples > 1 {
                        t.texture_type = TextureType::Type2DMultisample;''', '''                    let ttype = if p.len() >= 36 { u32at(&p, 24) } else { 2 };
                    if samples <= 1 && p.len() >= 36 {
                        let tt = TextureType::try_from(ttype)?;
                        t.texture_type = tt;
                        t.depth = u32at(&p, 28).max(1) as u64;
                        t.array_length = u32at(&p, 32).max(1) as u64;
                    }
                    if samples > 1 {
                        t.texture_type = TextureType::Type2DMultisample;''')
r = rep(r, '                    texs.insert(next, (device.new_texture(&t)?, tw, th));', '                    texs.insert(next, (device.new_texture(&t)?, tw, th));\n                    ttypes.insert(next, ttype);')
# op 10: slice/z and rows semantics
r = rep(r, '''                    let (y0, th) = if p.len() >= 16 { (u32at(&p, 8) as u64, u32at(&p, 12) as u64) } else { (0, th) };''',
        '''                    let (y0, th) = if p.len() >= 16 && u32at(&p, 12) != 0 { (u32at(&p, 8) as u64, u32at(&p, 12) as u64) } else { (0, th) };
                    let kind = ttypes.get(&u32at(&p, 0)).copied().unwrap_or(2);
                    let (rslice, rz) = if p.len() >= 24 { (u32at(&p, 16) as u64, u32at(&p, 20) as u64) } else { (0, 0) };
                    let (rslice, rz) = if kind == 7 { (0, rz) } else { (rslice, 0) };''')
r = rep(r, 'blit.copy_from_texture_to_buffer(tex, 0, level, Origin::new(0, y0, 0), Size::new(tw, th, 1), &rb, 0, row, row * th, BlitOption::None)?;',
        'blit.copy_from_texture_to_buffer(tex, rslice, level, Origin::new(0, y0, rz), Size::new(tw, th, 1), &rb, 0, row, row * th, BlitOption::None)?;')
# op 13
r = rep(r, '''                    let level = u32at(&p, 20) as u64;
                    let (tw, th) = ((*tw >> level).max(1), (*th >> level).max(1));
                    let need = (w * h * 4) as usize;
                    if x + w > tw || y + h > th || p.len() < 24 + need {
                        return Err("texture region does not fit".into());
                    }
                    let sb = device.new_buffer_with_bytes(&p[24..24 + need], ResourceOptions::shared())?;''', '''                    let level = u32at(&p, 20) as u64;
                    let (wslice, wz) = (u32at(&p, 24) as u64, u32at(&p, 28) as u64);
                    let kind = ttypes.get(&u32at(&p, 0)).copied().unwrap_or(2);
                    let (wslice, wz) = if kind == 7 { (0, wz) } else { (wslice, 0) };
                    let (tw, th) = ((*tw >> level).max(1), (*th >> level).max(1));
                    let need = (w * h * 4) as usize;
                    if x + w > tw || y + h > th || p.len() < 32 + need {
                        return Err("texture region does not fit".into());
                    }
                    let sb = device.new_buffer_with_bytes(&p[32..32 + need], ResourceOptions::shared())?;''')
r = rep(r, 'Size::new(w, h, 1), tex, 0, level, Origin::new(x, y, 0), BlitOption::None)?;', 'Size::new(w, h, 1), tex, wslice, level, Origin::new(x, y, wz), BlitOption::None)?;')
r = rep(r, '''                        2 => {
                            texs.remove(&id);''', '''                        2 => {
                            ttypes.remove(&id);
                            texs.remove(&id);''') if '                        2 => {\n                            texs.remove(&id);' in r else r
open(srv, 'w', newline='\n').write(r)
print('patched')
