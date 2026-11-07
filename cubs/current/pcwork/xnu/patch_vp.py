#!/usr/bin/env python3
"""patch_vp.py MTLSHIM.M MTLSRV.RS: setViewport: and setScissorRect: reach the GPU. A trailing section of op 12 after the samplers: hasvp, viewport (6 x f32: x, y, w, h,
znear, zfar), hassc, scissor (4 x u32: x, y, w, h)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'hasvp' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


s = rep(s, '- (void)setViewport:(MTLViewport)viewport {}\n- (void)setScissorRect:(MTLScissorRect)rect {}', '''- (void)setViewport:(MTLViewport)v
{
	float f[6] = { (float)v.originX, (float)v.originY, (float)v.width, (float)v.height, (float)v.znear, (float)v.zfar };

	self.vpdata = [NSData dataWithBytes:f length:sizeof f];
}
- (void)setScissorRect:(MTLScissorRect)r
{
	uint32_t u[4] = { (uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.width, (uint32_t)r.height };

	self.scdata = [NSData dataWithBytes:u length:sizeof u];
}''')
s = rep(s, '@property (nonatomic, retain) NSMutableDictionary *fbound, *fsamp;', '@property (nonatomic, retain) NSMutableDictionary *fbound, *fsamp;\n@property (nonatomic, retain) NSData *vpdata, *scdata;')
s = rep(s, '@"dload": @(', '@"vp": self.vpdata ? (id)self.vpdata : (id)[NSNull null], @"sc": self.scdata ? (id)self.scdata : (id)[NSNull null], @"dload": @(')
s = rep(s, '''			for (NSNumber *k in fskeys) {
				uint32_t e[2] = { ((MSSampler *)fsamp[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
''', '''			for (NSNumber *k in fskeys) {
				uint32_t e[2] = { ((MSSampler *)fsamp[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
			NSData *vpd = [extra[@"vp"] isKindOfClass:[NSData class]] ? extra[@"vp"] : nil, *scd = [extra[@"sc"] isKindOfClass:[NSData class]] ? extra[@"sc"] : nil;
			uint32_t hasvp = vpd ? 1 : 0, hassc = scd ? 1 : 0, zero6[6] = { 0 }, zero4[4] = { 0 };

			[call appendBytes:&hasvp length:4];
			[call appendBytes:vpd ? vpd.bytes : zero6 length:24];
			[call appendBytes:&hassc length:4];
			[call appendBytes:scd ? scd.bytes : zero4 length:16];
''')
open(shim, 'w', newline='\n').write(s)

r = open(srv).read()
r = rep(r, '''                    let pt = match prim {
                        0 => PrimitiveType::Point,''', '''                    {
                        let so = 88 + nvb * 12 + nft * 8 + nfb * 12;
                        let nfs = if p.len() >= so + 4 { u32at(&p, so) as usize } else { 0 };
                        let vo = so + 4 + nfs * 8;
                        if p.len() >= vo + 48 {
                            let fv = |i: usize| f32::from_bits(u32at(&p, vo + 4 + i * 4)) as f64;
                            if u32at(&p, vo) != 0 {
                                let mut vp = Viewport::default();
                                vp.origin_x = fv(0);
                                vp.origin_y = fv(1);
                                vp.width = fv(2);
                                vp.height = fv(3);
                                vp.znear = fv(4);
                                vp.zfar = fv(5);
                                enc.set_viewport(vp)?;
                            }
                            if u32at(&p, vo + 28) != 0 {
                                let mut sc = ScissorRect::default();
                                sc.x = u32at(&p, vo + 32) as u64;
                                sc.y = u32at(&p, vo + 36) as u64;
                                sc.width = u32at(&p, vo + 40) as u64;
                                sc.height = u32at(&p, vo + 44) as u64;
                                enc.set_scissor_rect(sc)?;
                            }
                        }
                    }
                    let pt = match prim {
                        0 => PrimitiveType::Point,''')
open(srv, 'w', newline='\n').write(r)
print('patched')
