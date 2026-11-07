#!/usr/bin/env python3
"""patch_shim_layer.py MTLSHIM.M: CAMetalLayer support for libmtlshim: -[CAMetalLayer nextDrawable] gives shim textures, a
command buffer's presentDrawable: shows the texture as the layer's contents (read back over the RPC), so an ordinary Metal app
(device from MTLCreateSystemDefaultDevice, a CAMetalLayer in its layer tree) draws on the PowerVR and CoreAnimation composites it."""
import sys

path = sys.argv[1]
s = open(path).read()
if 'MSDrawable' in s:
    print('already patched')
    sys.exit(0)

s = s.replace('#import <Metal/Metal.h>\n', '#import <Metal/Metal.h>\n#import <QuartzCore/QuartzCore.h>\n#import <QuartzCore/CAMetalLayer.h>\n#include <objc/runtime.h>\n', 1)

s = s.replace('@interface MSRenderPipeline : NSObject <MTLRenderPipelineState>',
'''@interface MSDrawable : NSObject <CAMetalDrawable>
@property (nonatomic, retain) MSTexture *tex;
@property (nonatomic, assign) CAMetalLayer *lay;
@property (nonatomic) NSUInteger did;
@end
@interface MSRenderPipeline : NSObject <MTLRenderPipelineState>''', 1)

s = s.replace('@property (nonatomic) MTLCommandBufferStatus st;\n@end',
'''@property (nonatomic) MTLCommandBufferStatus st;
@property (nonatomic, retain) NSMutableArray *presents;
@property (nonatomic, retain) NSMutableArray *handlers;
@end''', 1)

old = '\t}\n}\n- (void)waitUntilCompleted {}\n'
assert old in s
new = r'''	}
	[self finishPresents];
}
- (void)finishPresents
{
	for (id<CAMetalDrawable> d in self.presents) {
		[d present];
	}
	[self.presents removeAllObjects];
	for (void (^h)(id<MTLCommandBuffer>) in self.handlers) {
		h(self);
	}
	[self.handlers removeAllObjects];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable
{
	if (!self.presents) {
		self.presents = [NSMutableArray array];
	}
	[self.presents addObject:drawable];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t { [self presentDrawable:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)t { [self presentDrawable:drawable]; }
- (void)addCompletedHandler:(void (^)(id<MTLCommandBuffer>))block
{
	if (!self.handlers) {
		self.handlers = [NSMutableArray array];
	}
	[self.handlers addObject:[[block copy] autorelease]];
}
- (void)addScheduledHandler:(void (^)(id<MTLCommandBuffer>))block { block(self); }
- (void)enqueue {}
- (void)waitUntilCompleted {}
'''
s = s.replace(old, new, 1)

marker = '/* ------------------------------------------------------------------------------------------------ entry points */'
assert marker in s
layer_code = r'''/* ------------------------------------------------------------------------------------------------ CAMetalLayer */

static const char *kDeviceKey = "ms.device";
static const char *kPoolKey = "ms.pool";
static const char *kCursorKey = "ms.cursor";

@implementation MSDrawable
- (id<MTLTexture>)texture { return self.tex; }
- (CAMetalLayer *)layer { return self.lay; }
- (NSUInteger)drawableID { return self.did; }
- (CFTimeInterval)presentedTime { return 0; }
- (void)addPresentedHandler:(void (^)(id<MTLDrawable>))block { block(self); }
- (void)present
{
	MSTexture *t = self.tex;
	NSUInteger w = t.w, h = t.h;
	uint32_t id = t.rid, n = 0;
	uint8_t *px = malloc(w * h * 4);

	if (w * h * 4 > PAYLOAD_MAX) {
		free(px);
		return;
	}
	if (rpc_call(10, &id, 4, NULL, px, (uint32_t)(w * h * 4), &n) == 0 && n == w * h * 4) {
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CFDataRef data = CFDataCreate(NULL, px, (CFIndex)n);
		CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
		CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast, dp, NULL, false,
		    kCGRenderingIntentDefault);
		CAMetalLayer *layer = [self.lay retain];

		dispatch_async(dispatch_get_main_queue(), ^{
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			layer.contents = (id)img;
			[CATransaction commit];
			CGImageRelease(img);
			[layer release];
		});
		CGDataProviderRelease(dp);
		CFRelease(data);
		CGColorSpaceRelease(cs);
	}
	free(px);
}
- (void)presentAtTime:(CFTimeInterval)t { [self present]; }
- (void)presentAfterMinimumDuration:(CFTimeInterval)t { [self present]; }
@end

static id
ms_layer_nextDrawable(id self, SEL _cmd)
{
	CAMetalLayer *l = self;
	CGSize size = l.drawableSize;

	if (size.width < 1 || size.height < 1) {
		CGFloat scale = l.contentsScale > 0 ? l.contentsScale : 1;

		size = CGSizeMake(l.bounds.size.width * scale, l.bounds.size.height * scale);
	}
	NSUInteger w = (NSUInteger)size.width, h = (NSUInteger)size.height;
	NSMutableArray *pool = objc_getAssociatedObject(l, kPoolKey);
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();

	if (!dev || w < 1 || h < 1 || w * h * 4 > PAYLOAD_MAX) {
		return nil;
	}
	if (!pool || [(MSTexture *)pool.firstObject w] != w || [(MSTexture *)pool.firstObject h] != h) {
		pool = [NSMutableArray array];
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];

		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		for (int i = 0; i < 3; i++) {
			id<MTLTexture> t = [dev newTextureWithDescriptor:td];

			if (t) {
				[pool addObject:t];
			}
		}
		objc_setAssociatedObject(l, kPoolKey, pool, OBJC_ASSOCIATION_RETAIN);
	}
	if (pool.count == 0) {
		return nil;
	}
	NSUInteger cursor = [objc_getAssociatedObject(l, kCursorKey) unsignedIntegerValue];
	MSDrawable *d = [[[MSDrawable alloc] init] autorelease];

	d.tex = pool[cursor % pool.count];
	d.lay = l;
	d.did = cursor;
	objc_setAssociatedObject(l, kCursorKey, @(cursor + 1), OBJC_ASSOCIATION_RETAIN);
	return d;
}

static void
ms_layer_setDevice(id self, SEL _cmd, id device)
{
	objc_setAssociatedObject(self, kDeviceKey, device, OBJC_ASSOCIATION_RETAIN);
}

static id
ms_layer_device(id self, SEL _cmd)
{
	return objc_getAssociatedObject(self, kDeviceKey);
}

static void
ms_install_layer_support(void)
{
	Class c = NSClassFromString(@"CAMetalLayer");

	if (!c) {
		return;
	}
	class_replaceMethod(c, @selector(nextDrawable), (IMP)ms_layer_nextDrawable, "@@:");
	class_replaceMethod(c, @selector(setDevice:), (IMP)ms_layer_setDevice, "v@:@");
	class_replaceMethod(c, @selector(device), (IMP)ms_layer_device, "@@:");
	class_replaceMethod(c, NSSelectorFromString(@"preferredDevice"), (IMP)ms_layer_device, "@@:");
}

'''
s = s.replace(marker, layer_code + marker, 1)

s = s.replace('\t\tg_lock = [[NSLock alloc] init];\n\t\tif (rpc_open()) {\n\t\t\tdev = [[MSDevice alloc] init];\n\t\t}',
              '\t\tg_lock = [[NSLock alloc] init];\n\t\tif (rpc_open()) {\n\t\t\tdev = [[MSDevice alloc] init];\n\t\t\tms_install_layer_support();\n\t\t}', 1)
assert 'ms_install_layer_support();' in s
open(path, 'w', newline='\n').write(s)
print('patched')
