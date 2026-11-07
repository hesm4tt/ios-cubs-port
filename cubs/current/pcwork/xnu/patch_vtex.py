#!/usr/bin/env python3
"""patch_vtex.py MTLSHIM.M MTLSRV.RS: vertex-stage textures and samplers. A trailing section of op 12 after the viewport/scissor words: nvt, nvt x (texture id, index),
nvs, nvs x (sampler id, index)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'vtex' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


s = rep(s, '- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index\n', '''- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
	if (texture) {
		self.vtex[@(index)] = texture;
	} else {
		[self.vtex removeObjectForKey:@(index)];
	}
}
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (sampler) {
		self.vsamp[@(index)] = sampler;
	} else {
		[self.vsamp removeObjectForKey:@(index)];
	}
}
- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
''')
s = rep(s, '@property (nonatomic, retain) NSData *vpdata, *scdata;', '@property (nonatomic, retain) NSData *vpdata, *scdata;\n@property (nonatomic, retain) NSMutableDictionary *vtex, *vsamp;')
s = rep(s, '\te.fsamp = [NSMutableDictionary dictionary];', '\te.fsamp = [NSMutableDictionary dictionary];\n\te.vtex = [NSMutableDictionary dictionary];\n\te.vsamp = [NSMutableDictionary dictionary];')
s = rep(s, '@"vp": self.vpdata', '@"vtex": [self.vtex copy], @"vsamp": [self.vsamp copy], @"vp": self.vpdata')
s = rep(s, '''			[call appendBytes:&hassc length:4];
			[call appendBytes:scd ? scd.bytes : zero4 length:16];
''', '''			[call appendBytes:&hassc length:4];
			[call appendBytes:scd ? scd.bytes : zero4 length:16];
			NSDictionary *vt = extra[@"vtex"], *vs = extra[@"vsamp"];
			uint32_t nvt = (uint32_t)vt.count, nvs = (uint32_t)vs.count;

			[call appendBytes:&nvt length:4];
			for (NSNumber *k in [vt allKeys]) {
				uint32_t e[2] = { ((MSTexture *)vt[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
			[call appendBytes:&nvs length:4];
			for (NSNumber *k in [vs allKeys]) {
				uint32_t e[2] = { ((MSSampler *)vs[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
''')
open(shim, 'w', newline='\n').write(s)

r = open(srv).read()
r = rep(r, '''                                enc.set_scissor_rect(sc)?;
                            }
''', '''                                enc.set_scissor_rect(sc)?;
                            }
                            let mut o = vo + 48;
                            if p.len() >= o + 4 {
                                let nvt = u32at(&p, o) as usize;
                                o += 4;
                                for k in 0..nvt {
                                    let (t, _, _) = texs.get(&u32at(&p, o + k * 8)).ok_or("no such vertex texture")?;
                                    enc.set_vertex_texture(Some(t), u32at(&p, o + k * 8 + 4))?;
                                }
                                o += nvt * 8;
                            }
                            if p.len() >= o + 4 {
                                let nvs = u32at(&p, o) as usize;
                                o += 4;
                                for k in 0..nvs {
                                    let sm = samplers.get(&u32at(&p, o + k * 8)).ok_or("no such vertex sampler")?;
                                    enc.set_vertex_sampler_state(Some(sm), u32at(&p, o + k * 8 + 4))?;
                                }
                            }
''')
open(srv, 'w', newline='\n').write(r)
print('patched')
