#!/usr/bin/env python3
"""patch_blit.py MTLSHIM.M: blit copies in the shim's blit encoder, run in order at commit through the texture and buffer byte interfaces: buffer <-> texture,
texture -> texture, buffer -> buffer, fillBuffer."""
import sys

shim = sys.argv[1]
s = open(shim).read()
if 'copyFromBuffer:(id<MTLBuffer>)sb' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


s = rep(s, '''- (void)endEncoding {}
- (void)setLabel:(NSString *)l {}
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSCommandBuffer''', '''- (void)queue:(void (^)(void))blk
{
	[self.cb.dispatches addObject:@{ @"type": @"blk", @"blk": [[blk copy] autorelease] }];
}
- (void)copyFromBuffer:(id<MTLBuffer>)sb sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi
	    sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dt destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
	    destinationOrigin:(MTLOrigin)origin
{
	[self queue:^{
		MTLRegion r = MTLRegionMake2D(origin.x, origin.y, size.width, size.height);

		[dt replaceRegion:r mipmapLevel:level withBytes:(const uint8_t *)[sb contents] + so bytesPerRow:bpr];
	}];
}
- (void)copyFromBuffer:(id<MTLBuffer>)sb sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi
	    sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dt destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
	    destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options
{
	[self copyFromBuffer:sb sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:size toTexture:dt destinationSlice:slice
	    destinationLevel:level destinationOrigin:origin];
}
- (void)copyFromTexture:(id<MTLTexture>)st sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size
	       toBuffer:(id<MTLBuffer>)db destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	[self queue:^{
		MTLRegion r = MTLRegionMake2D(origin.x, origin.y, size.width, size.height);

		[st getBytes:(uint8_t *)[db contents] + doff bytesPerRow:bpr fromRegion:r mipmapLevel:level];
	}];
}
- (void)copyFromTexture:(id<MTLTexture>)st sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size
	       toBuffer:(id<MTLBuffer>)db destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
	       options:(MTLBlitOption)options
{
	[self copyFromTexture:st sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:db destinationOffset:doff
	    destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromTexture:(id<MTLTexture>)st sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size
	      toTexture:(id<MTLTexture>)dt destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorigin
{
	[self queue:^{
		NSMutableData *tmp = [NSMutableData dataWithLength:size.width * size.height * 4];

		[st getBytes:tmp.mutableBytes bytesPerRow:size.width * 4 fromRegion:MTLRegionMake2D(so.x, so.y, size.width, size.height) mipmapLevel:sl];
		[dt replaceRegion:MTLRegionMake2D(dorigin.x, dorigin.y, size.width, size.height) mipmapLevel:dl withBytes:tmp.bytes bytesPerRow:size.width * 4];
	}];
}
- (void)copyFromTexture:(id<MTLTexture>)st toTexture:(id<MTLTexture>)dt
{
	[self copyFromTexture:st sourceSlice:0 sourceLevel:0 sourceOrigin:(MTLOrigin){ 0, 0, 0 } sourceSize:(MTLSize){ st.width, st.height, 1 } toTexture:dt
	    destinationSlice:0 destinationLevel:0 destinationOrigin:(MTLOrigin){ 0, 0, 0 }];
}
- (void)copyFromBuffer:(id<MTLBuffer>)sb sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)db destinationOffset:(NSUInteger)doff size:(NSUInteger)size
{
	[self queue:^{
		memmove((uint8_t *)[db contents] + doff, (const uint8_t *)[sb contents] + so, size);
	}];
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
{
	[self queue:^{
		memset((uint8_t *)[buffer contents] + range.location, value, range.length);
	}];
}
- (void)synchronizeResource:(id<MTLResource>)resource {}
- (void)endEncoding {}
- (void)setLabel:(NSString *)l {}
- (NSString *)label { return nil; }
- (id<MTLDevice>)device { return nil; }
@end

@implementation MSCommandBuffer''')
s = rep(s, '''		if ([d[@"type"] isEqual:@"mips"]) {''', '''		if ([d[@"type"] isEqual:@"blk"]) {
			void (^blk)(void) = d[@"blk"];

			blk();
			continue;
		}
		if ([d[@"type"] isEqual:@"mips"]) {''')
open(shim, 'w', newline='\n').write(s)
print('patched')
