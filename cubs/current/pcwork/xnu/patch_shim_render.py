"""Add the render path (textures, render pipelines, render encoder) to mtlshim.m."""
s = open('mtlshim.m').read()
if 'MSRenderEncoder' in s:
    raise SystemExit('already patched')

s = s.replace('''@interface MSComputeEncoder : NSObject <MTLComputeCommandEncoder>''', '''@interface MSTexture : NSObject <MTLTexture>
@property (nonatomic) uint32_t rid;
@property (nonatomic) NSUInteger w, h;
@end
@interface MSRenderPipeline : NSObject <MTLRenderPipelineState>
@property (nonatomic) uint32_t rid;
@end
@interface MSRenderEncoder : NSObject <MTLRenderCommandEncoder>
@property (nonatomic, assign) MSCommandBuffer *cb;
@property (nonatomic, retain) MSRenderPipeline *pso;
@property (nonatomic, retain) MSTexture *target;
@property (nonatomic) float clear[4];
@property (nonatomic, retain) NSMutableDictionary *bound;
@end
@interface MSComputeEncoder : NSObject <MTLComputeCommandEncoder>''', 1)

s = s.replace('''- (BOOL)isLowPower { return NO; }''', '''- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
	uint32_t p[2] = { (uint32_t)d.width, (uint32_t)d.height }, rid = 0;

	if (rpc_call(7, p, sizeof p, &rid, NULL, 0, NULL) != 0) {
		return nil;
	}
	MSTexture *t = [[MSTexture alloc] init];

	t.rid = rid;
	t.w = d.width;
	t.h = d.height;
	return t;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
	MSFunction *vf = (MSFunction *)d.vertexFunction, *ff = (MSFunction *)d.fragmentFunction;
	NSData *vn = [vf.fname dataUsingEncoding:NSUTF8StringEncoding], *fnn = [ff.fname dataUsingEncoding:NSUTF8StringEncoding];
	NSMutableData *p = [NSMutableData data];
	uint32_t lib = vf.lib.rid, vl = (uint32_t)vn.length, rid = 0;

	[p appendBytes:&lib length:4];
	[p appendBytes:&vl length:4];
	[p appendData:vn];
	[p appendData:fnn];
	if (rpc_call(8, p.bytes, (uint32_t)p.length, &rid, NULL, 0, NULL) != 0) {
		if (error) {
			*error = [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:3 userInfo:nil];
		}
		return nil;
	}
	MSRenderPipeline *r = [[MSRenderPipeline alloc] init];

	r.rid = rid;
	return r;
}
- (BOOL)isLowPower { return NO; }''', 1)

s = s.replace('''- (void)commit
{
	self.st = MTLCommandBufferStatusCompleted;
	for (NSDictionary *d in self.dispatches) {
		MSPipeline *pso = d[@"pso"];''', '''- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)desc
{
	MSRenderEncoder *e = [[MSRenderEncoder alloc] init];
	MTLRenderPassColorAttachmentDescriptor *c = desc.colorAttachments[0];

	e.cb = self;
	e.target = (MSTexture *)c.texture;
	e.clear[0] = (float)c.clearColor.red;
	e.clear[1] = (float)c.clearColor.green;
	e.clear[2] = (float)c.clearColor.blue;
	e.clear[3] = (float)c.clearColor.alpha;
	e.bound = [NSMutableDictionary dictionary];
	return e;
}
- (void)commit
{
	self.st = MTLCommandBufferStatusCompleted;
	for (NSDictionary *d in self.dispatches) {
		if ([d[@"type"] isEqual:@"render"]) {
			MSRenderPipeline *rp = d[@"pso"];
			MSTexture *tex = d[@"tex"];
			NSDictionary *bound = d[@"bound"];
			NSArray *keys = [[bound allKeys] sortedArrayUsingSelector:@selector(compare:)];
			NSMutableData *call = [NSMutableData data];
			uint32_t h0[2] = { rp.rid, tex.rid };
			float *cl = (float *)[d[@"clear"] bytes];
			uint32_t h1[4], h2[4] = { (uint32_t)keys.count, [d[@"prim"] unsignedIntValue], [d[@"start"] unsignedIntValue],
						  [d[@"count"] unsignedIntValue] };

			memcpy(h1, cl, 16);
			[call appendBytes:h0 length:8];
			[call appendBytes:h1 length:16];
			[call appendBytes:h2 length:16];
			for (NSNumber *k in keys) {
				MSBuffer *b = bound[k][@"buf"];
				uint32_t w[2] = { b.rid, 0 };
				NSMutableData *wd = [NSMutableData dataWithBytes:w length:8];

				[wd appendBytes:b.mem length:b.len];
				(void)rpc_call(3, wd.bytes, (uint32_t)wd.length, NULL, NULL, 0, NULL);
				uint32_t e[3] = { b.rid, (uint32_t)k.unsignedIntValue, (uint32_t)[bound[k][@"off"] unsignedIntValue] };

				[call appendBytes:e length:sizeof e];
			}
			if (rpc_call(9, call.bytes, (uint32_t)call.length, NULL, NULL, 0, NULL) != 0) {
				self.st = MTLCommandBufferStatusError;
			}
			continue;
		}
		MSPipeline *pso = d[@"pso"];''', 1)

s = s.replace('''/* ------------------------------------------------------------------------------------------------ entry points */''', '''@implementation MSTexture
- (NSUInteger)width { return self.w; }
- (NSUInteger)height { return self.h; }
- (NSUInteger)depth { return 1; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatRGBA8Unorm; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (void)getBytes:(void *)pixelBytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
{
	uint32_t id = self.rid, n = 0;
	uint8_t *all = malloc(self.w * self.h * 4);

	if (rpc_call(10, &id, 4, NULL, all, (uint32_t)(self.w * self.h * 4), &n) == 0 && n == self.w * self.h * 4) {
		for (NSUInteger y = 0; y < region.size.height; y++) {
			memcpy((uint8_t *)pixelBytes + y * bytesPerRow, all + ((region.origin.y + y) * self.w + region.origin.x) * 4,
			    region.size.width * 4);
		}
	}
	free(all);
}
- (NSString *)label { return nil; }
- (void)setLabel:(NSString *)l {}
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSRenderPipeline
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState { self.pso = (MSRenderPipeline *)pipelineState; }
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	self.bound[@(index)] = @{ @"buf": buffer, @"off": @(offset) };
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount
{
	[self.cb.dispatches addObject:@{ @"type": @"render", @"pso": self.pso, @"tex": self.target, @"bound": [self.bound copy],
					 @"clear": [NSData dataWithBytes:self.clear length:16], @"prim": @((uint32_t)primitiveType),
					 @"start": @(vertexStart), @"count": @(vertexCount) }];
}
- (void)endEncoding {}
- (NSString *)label { return nil; }
- (void)setLabel:(NSString *)l {}
- (id<MTLDevice>)device { return nil; }
@end

/* ------------------------------------------------------------------------------------------------ entry points */''', 1)
open('mtlshim.m', 'w', newline='\n').write(s)
print('patched', s.count('MSRenderEncoder'))
