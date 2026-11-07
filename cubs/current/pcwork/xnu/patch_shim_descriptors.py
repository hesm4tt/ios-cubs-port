"""Add the descriptor classes a render program needs (texture, render pipeline, render pass) to mtlshim.m, so that a Metal
program can be linked with the shim alone and never has to load Metal.framework."""
s = open('mtlshim.m').read()
if '@implementation MTLTextureDescriptor' in s:
    raise SystemExit('already patched')

block = r'''
/* ------------------------------------------------------------------------------------------------ descriptors
 * The public descriptor classes, implemented here so that a program linked with this library alone needs no Metal.framework.
 * Only the properties a simple render program uses are implemented. */

@implementation MTLTextureDescriptor
@synthesize pixelFormat, width, height, usage;
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:(NSUInteger)w height:(NSUInteger)h mipmapped:(BOOL)m
{
	MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];

	d.pixelFormat = pf;
	d.width = w;
	d.height = h;
	return d;
}
@end

@implementation MTLRenderPipelineColorAttachmentDescriptor
@synthesize pixelFormat;
@end

@implementation MTLRenderPipelineColorAttachmentDescriptorArray {
	NSMutableDictionary *items;
}
- (id)objectAtIndexedSubscript:(NSUInteger)i
{
	if (!items) {
		items = [[NSMutableDictionary alloc] init];
	}
	id o = items[@(i)];

	if (!o) {
		o = [[MTLRenderPipelineColorAttachmentDescriptor alloc] init];
		items[@(i)] = o;
	}
	return o;
}
@end

@implementation MTLRenderPipelineDescriptor {
	MTLRenderPipelineColorAttachmentDescriptorArray *colors;
}
@synthesize vertexFunction, fragmentFunction;
- (MTLRenderPipelineColorAttachmentDescriptorArray *)colorAttachments
{
	if (!colors) {
		colors = [[MTLRenderPipelineColorAttachmentDescriptorArray alloc] init];
	}
	return colors;
}
@end

@implementation MTLRenderPassColorAttachmentDescriptor
@synthesize texture, loadAction, storeAction, clearColor;
@end

@implementation MTLRenderPassColorAttachmentDescriptorArray {
	NSMutableDictionary *items;
}
- (id)objectAtIndexedSubscript:(NSUInteger)i
{
	if (!items) {
		items = [[NSMutableDictionary alloc] init];
	}
	id o = items[@(i)];

	if (!o) {
		o = [[MTLRenderPassColorAttachmentDescriptor alloc] init];
		items[@(i)] = o;
	}
	return o;
}
@end

@implementation MTLRenderPassDescriptor {
	MTLRenderPassColorAttachmentDescriptorArray *colors;
}
+ (MTLRenderPassDescriptor *)renderPassDescriptor
{
	return [[MTLRenderPassDescriptor alloc] init];
}
- (MTLRenderPassColorAttachmentDescriptorArray *)colorAttachments
{
	if (!colors) {
		colors = [[MTLRenderPassColorAttachmentDescriptorArray alloc] init];
	}
	return colors;
}
@end

'''
marker = '/* ------------------------------------------------------------------------------------------------ entry points */'
assert marker in s
s = s.replace(marker, block + marker, 1)
open('mtlshim.m', 'w', newline='\n').write(s)
print('patched')
