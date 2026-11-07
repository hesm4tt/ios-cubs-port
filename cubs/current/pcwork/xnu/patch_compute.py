#!/usr/bin/env python3
"""patch_compute.py MTLSHIM.M MTLSRV.RS: the rest of the everyday compute encoder: setBytes:length:atIndex:, setBufferOffset:atIndex:, setTexture:atIndex: (read and
write textures: op 7's usage word bit 1 = ShaderWrite), setThreadgroupMemoryLength:atIndex:, dispatchThreadgroupsWithIndirectBuffer: (arguments read from the host copy
when encoded), setSamplerState (kept as a no-op). Op 6 DISPATCH gets a trailing section after its buffers: nt, nt x (texture id, index), ntg, ntg x (length, index)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'setThreadgroupMemoryLength' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


# ---- texture usage: ShaderWrite
s = rep(s, '(d.usage & MTLTextureUsageShaderRead) ? 1u : 0u,', '((d.usage & MTLTextureUsageShaderRead) ? 1u : 0u) | ((d.usage & MTLTextureUsageShaderWrite) ? 2u : 0u),')

# ---- the compute encoder
s = rep(s, '''- (void)record:(MTLSize)grid group:(MTLSize)tg
{
	[self.cb.dispatches addObject:@{ @"pso": self.pso, @"bound": [self.bound copy],''', '''- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	NSDictionary *b = self.bound[@(index)];

	if (b) {
		self.bound[@(index)] = @{ @"buf": b[@"buf"], @"off": @(offset) };
	}
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id<MTLBuffer> b = [MTLCreateSystemDefaultDevice() newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];

	self.bound[@(index)] = @{ @"buf": b, @"off": @0 };
	[b release];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
	if (!self.ctex) {
		self.ctex = [NSMutableDictionary dictionary];
	}
	if (texture) {
		self.ctex[@(index)] = texture;
	} else {
		[self.ctex removeObjectForKey:@(index)];
	}
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (!self.tgmem) {
		self.tgmem = [NSMutableDictionary dictionary];
	}
	self.tgmem[@(index)] = @(length);
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)tg
{
	const uint32_t *a = (const uint32_t *)((const uint8_t *)[indirectBuffer contents] + offset);

	[self dispatchThreadgroups:(MTLSize){ a[0], a[1], a[2] } threadsPerThreadgroup:tg];
}
- (void)record:(MTLSize)grid group:(MTLSize)tg
{
	[self.cb.dispatches addObject:@{ @"pso": self.pso, @"bound": [self.bound copy],
					 @"ctex": self.ctex ? [[self.ctex copy] autorelease] : (id)[NSNull null],
					 @"tgmem": self.tgmem ? [[self.tgmem copy] autorelease] : (id)[NSNull null],''')
s = rep(s, '@interface MSComputeEncoder : NSObject <MTLComputeCommandEncoder>', '@interface MSComputeEncoder : NSObject <MTLComputeCommandEncoder>\n@property (nonatomic, retain) NSMutableDictionary *ctex, *tgmem;')

# ---- commit: the trailing section of op 6
s = rep(s, '''		if (rpc_call(6, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {''', '''		NSDictionary *ct = [d[@"ctex"] isKindOfClass:[NSDictionary class]] ? d[@"ctex"] : nil;
		NSDictionary *tm = [d[@"tgmem"] isKindOfClass:[NSDictionary class]] ? d[@"tgmem"] : nil;
		uint32_t nct = (uint32_t)ct.count, ntm = (uint32_t)tm.count;

		[call appendBytes:&nct length:4];
		for (NSNumber *k in [ct allKeys]) {
			uint32_t e[2] = { ((MSTexture *)ct[k]).rid, (uint32_t)k.unsignedIntValue };

			[call appendBytes:e length:sizeof e];
		}
		[call appendBytes:&ntm length:4];
		for (NSNumber *k in [tm allKeys]) {
			uint32_t e[2] = { (uint32_t)[tm[k] unsignedIntValue], (uint32_t)k.unsignedIntValue };

			[call appendBytes:e length:sizeof e];
		}
		if (rpc_call(6, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {''')
open(shim, 'w', newline='\n').write(s)

r = open(srv).read()
r = rep(r, '''                    enc.dispatch_threads(grid, tg)?;''', '''                    {
                        let mut o = 32 + nb * 12;
                        if p.len() >= o + 4 {
                            let nt = u32at(&p, o) as usize;
                            o += 4;
                            for k in 0..nt {
                                let (t, _, _) = texs.get(&u32at(&p, o + k * 8)).ok_or("no such compute texture")?;
                                enc.set_texture(Some(t), u32at(&p, o + k * 8 + 4))?;
                            }
                            o += nt * 8;
                        }
                        if p.len() >= o + 4 {
                            let ntg = u32at(&p, o) as usize;
                            o += 4;
                            for k in 0..ntg {
                                enc.set_threadgroup_memory_length(u32at(&p, o + k * 8) as u64, u32at(&p, o + k * 8 + 4) as u64)?;
                            }
                        }
                    }
                    enc.dispatch_threads(grid, tg)?;''')
r = rep(r, '''                    t.usage = if p.len() >= 12 && u32at(&p, 8) & 1 != 0 { TextureUsage::RENDER_TARGET | TextureUsage::SHADER_READ } else { TextureUsage::RENDER_TARGET };''',
        '''                    t.usage = if p.len() >= 12 && u32at(&p, 8) & 1 != 0 { TextureUsage::RENDER_TARGET | TextureUsage::SHADER_READ } else { TextureUsage::RENDER_TARGET };
                    if p.len() >= 12 && u32at(&p, 8) & 2 != 0 {
                        t.usage = t.usage | TextureUsage::SHADER_WRITE;
                    }''')
open(srv, 'w', newline='\n').write(r)
print('patched')
