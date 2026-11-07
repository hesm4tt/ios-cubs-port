#!/usr/bin/env python3
"""patch_sampler.py MTLSHIM.M MTLSRV.RS: real sampler states. Op 17 NEW_SAMPLER takes 12 words: min filter, mag filter, mip filter, s/t/r address modes,
border color, normalized coordinates, lod min (f32), lod max (f32), max anisotropy, compare function; the reply is the sampler id (freed with op 14 kind 7).
Op 12 RENDER_EX gets a trailing section after the fragment buffers: nfs, then nfs x (sampler id, index)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'NEW_SAMPLER' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


s = rep(s, '''@interface MSSampler : NSObject <MTLSamplerState>
@end
@implementation MSSampler
''', '''@interface MSSampler : NSObject <MTLSamplerState>
@property (nonatomic) uint32_t rid;
@end
@implementation MSSampler
- (void)dealloc
{
	if (self.rid) {
		uint32_t a[2] = { 7, self.rid };

		(void)rpc_call(14, a, sizeof a, NULL, NULL, 0, NULL); /* NEW_SAMPLER's object */
	}
	[super dealloc];
}
''')
s = rep(s, '''- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	return [[MSSampler alloc] init];
}''', '''- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
	float lmin = d.lodMinClamp, lmax = d.lodMaxClamp;
	uint32_t p[12] = { (uint32_t)d.minFilter, (uint32_t)d.magFilter, (uint32_t)d.mipFilter, (uint32_t)d.sAddressMode, (uint32_t)d.tAddressMode,
			   (uint32_t)d.rAddressMode, (uint32_t)d.borderColor, d.normalizedCoordinates ? 1u : 0u, 0, 0, (uint32_t)d.maxAnisotropy,
			   (uint32_t)d.compareFunction }, rid = 0;

	memcpy(&p[8], &lmin, 4);
	memcpy(&p[9], &lmax, 4);
	if (rpc_call(17, p, sizeof p, &rid, NULL, 0, NULL) != 0) {
		return nil; /* NEW_SAMPLER */
	}
	MSSampler *st = [[MSSampler alloc] init];

	st.rid = rid;
	return st;
}''')
# descriptor properties: the shim's own MTLSamplerDescriptor class, if it has one, must carry the fields
if '@implementation MTLSamplerDescriptor' not in s:
    s = rep(s, '@implementation MTLDepthStencilDescriptor', '''@implementation MTLSamplerDescriptor
@synthesize minFilter, magFilter, mipFilter, sAddressMode, tAddressMode, rAddressMode, borderColor, normalizedCoordinates, lodMinClamp, lodMaxClamp,
    maxAnisotropy, compareFunction, supportArgumentBuffers, label;
- (instancetype)init
{
	if ((self = [super init])) {
		maxAnisotropy = 1;
		normalizedCoordinates = YES;
		lodMaxClamp = 3.4028234663852886e38f;
	}
	return self;
}
@end

@implementation MTLDepthStencilDescriptor''')

# encoder: record the samplers
s = rep(s, '- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index {}', '''- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (sampler) {
		self.fsamp[@(index)] = sampler;
	} else {
		[self.fsamp removeObjectForKey:@(index)];
	}
}
- (void)setFragmentSamplerState:(id)sampler lodMinClamp:(float)lmin lodMaxClamp:(float)lmax atIndex:(NSUInteger)index
{
	[self setFragmentSamplerState:sampler atIndex:index];
}''')
s = rep(s, '@property (nonatomic, retain) NSMutableDictionary *fbound;', '@property (nonatomic, retain) NSMutableDictionary *fbound, *fsamp;')
s = rep(s, '\te.fbound = [NSMutableDictionary dictionary];', '\te.fbound = [NSMutableDictionary dictionary];\n\te.fsamp = [NSMutableDictionary dictionary];')
s = s.replace('@"fbound": [self.fbound copy], @"extra"', '@"fbound": [self.fbound copy], @"fsamp": [self.fsamp copy], @"extra"')
s = rep(s, '''				uint32_t e[3] = { b.rid, (uint32_t)k.unsignedIntValue, (uint32_t)[fbound[k][@"off"] unsignedIntValue] };

				[call appendBytes:e length:sizeof e];
			}
''', '''				uint32_t e[3] = { b.rid, (uint32_t)k.unsignedIntValue, (uint32_t)[fbound[k][@"off"] unsignedIntValue] };

				[call appendBytes:e length:sizeof e];
			}
			NSDictionary *fsamp = d[@"fsamp"];
			NSArray *fskeys = [[fsamp allKeys] sortedArrayUsingSelector:@selector(compare:)];
			uint32_t nfs = (uint32_t)fskeys.count;

			[call appendBytes:&nfs length:4];
			for (NSNumber *k in fskeys) {
				uint32_t e[2] = { ((MSSampler *)fsamp[k]).rid, (uint32_t)k.unsignedIntValue };

				[call appendBytes:e length:sizeof e];
			}
''')
open(shim, 'w', newline='\n').write(s)

r = open(srv).read()
r = rep(r, 'use metal2vulkan::pipeline::', 'use metal2vulkan::bind::state::SamplerDescriptor;\nuse metal2vulkan::metal::{SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter};\nuse metal2vulkan::pipeline::')
r = rep(r, '    let mut dss: HashMap<u32, DepthStencilState> = HashMap::new();', '    let mut dss: HashMap<u32, DepthStencilState> = HashMap::new();\n    let mut samplers: HashMap<u32, SamplerState> = HashMap::new();')
r = rep(r, '''                    let pt = match prim {
                        0 => PrimitiveType::Point,
                        1 => PrimitiveType::Line,
                        2 => PrimitiveType::LineStrip,
                        4 => PrimitiveType::TriangleStrip,
                        _ => PrimitiveType::Triangle,
                    };
                    if idxbuf != 0 {''', '''                    let so = 88 + nvb * 12 + nft * 8 + nfb * 12;
                    if p.len() >= so + 4 {
                        let nfs = u32at(&p, so) as usize;
                        for k in 0..nfs {
                            let o = so + 4 + k * 8;
                            let sm = samplers.get(&u32at(&p, o)).ok_or("no such sampler")?;
                            enc.set_fragment_sampler_state(Some(sm), u32at(&p, o + 4))?;
                        }
                    }
                    let pt = match prim {
                        0 => PrimitiveType::Point,
                        1 => PrimitiveType::Line,
                        2 => PrimitiveType::LineStrip,
                        4 => PrimitiveType::TriangleStrip,
                        _ => PrimitiveType::Triangle,
                    };
                    if idxbuf != 0 {''')
r = rep(r, '''                        6 => {
                            dss.remove(&id);
                        }''', '''                        6 => {
                            dss.remove(&id);
                        }
                        7 => {
                            samplers.remove(&id);
                        }''')
r = rep(r, '                _ => return Err("unknown op".into()),', '''                17 => {
                    // NEW_SAMPLER
                    let mut d = SamplerDescriptor::new();
                    d.min_filter = SamplerMinMagFilter::try_from(u32at(&p, 0))?;
                    d.mag_filter = SamplerMinMagFilter::try_from(u32at(&p, 4))?;
                    d.mip_filter = SamplerMipFilter::try_from(u32at(&p, 8))?;
                    d.s_address_mode = SamplerAddressMode::try_from(u32at(&p, 12))?;
                    d.t_address_mode = SamplerAddressMode::try_from(u32at(&p, 16))?;
                    d.r_address_mode = SamplerAddressMode::try_from(u32at(&p, 20))?;
                    d.border_color = SamplerBorderColor::try_from(u32at(&p, 24))?;
                    d.normalized_coordinates = u32at(&p, 28) != 0;
                    d.lod_min_clamp = f32::from_bits(u32at(&p, 32));
                    d.lod_max_clamp = f32::from_bits(u32at(&p, 36));
                    d.max_anisotropy = u32at(&p, 40).max(1);
                    d.compare_function = CompareFunction::try_from(u32at(&p, 44))?;
                    samplers.insert(next, device.new_sampler_state(&d)?);
                    result = next;
                    next += 1;
                }
                _ => return Err("unknown op".into()),''')
open(srv, 'w', newline='\n').write(r)
print('patched')
