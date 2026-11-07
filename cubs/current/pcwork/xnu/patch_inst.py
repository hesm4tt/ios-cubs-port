#!/usr/bin/env python3
"""patch_inst.py MTLSHIM.M MTLSRV.RS: instanced draws (instanceCount, baseInstance, baseVertex) and indirect draws whose arguments are read from the buffer's
host memory when the draw is encoded. Op 12 gets three more words after the last trailing section (the sixth resolve word): instance count, base instance,
base vertex (i32)."""
import sys

shim, srv = sys.argv[1:3]
s = open(shim).read()
if 'pend_inst' in s:
    print('already patched')
    sys.exit(0)


def rep(text, old, new):
    assert old in text, old[:90]
    return text.replace(old, new, 1)


s = rep(s, '\tuint32_t clear_stencil, sref_f, sref_b;\n', '\tuint32_t clear_stencil, sref_f, sref_b;\n\tuint32_t pend_inst, pend_binst;\n\tint32_t pend_bvert;\n')

# the two base draws record the instance fields (and reset them)
s = rep(s, '''					 @"ftex": [self.ftex copy], @"fbound": [self.fbound copy], @"fsamp": [self.fsamp copy], @"extra": [self extraState] }];
	drawn++;
}''', '''					 @"ftex": [self.ftex copy], @"fbound": [self.fbound copy], @"fsamp": [self.fsamp copy], @"extra": [self extraState],
					 @"inst": @(pend_inst ? pend_inst : 1u), @"binst": @(pend_binst), @"bvert": @(pend_bvert) }];
	pend_inst = pend_binst = 0;
	pend_bvert = 0;
	drawn++;
}''')
s = rep(s, '''@"ibuf": indexBuffer, @"ioff": @(indexBufferOffset), @"itype": @((uint32_t)indexType) }];
	drawn++;
}''', '''@"ibuf": indexBuffer, @"ioff": @(indexBufferOffset), @"itype": @((uint32_t)indexType),
					 @"inst": @(pend_inst ? pend_inst : 1u), @"binst": @(pend_binst), @"bvert": @(pend_bvert) }];
	pend_inst = pend_binst = 0;
	pend_bvert = 0;
	drawn++;
}''')

# the instanced variants
s = rep(s, '''vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
{
	[self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount];
}''', '''vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
{
	pend_inst = (uint32_t)instanceCount;
	[self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount];
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
	  baseInstance:(NSUInteger)baseInstance
{
	pend_binst = (uint32_t)baseInstance;
	[self drawPrimitives:primitiveType vertexStart:vertexStart vertexCount:vertexCount instanceCount:instanceCount];
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType indirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)offset
{
	/* MTLDrawPrimitivesIndirectArguments: vertexCount, instanceCount, vertexStart, baseInstance (read from the host copy) */
	const uint32_t *a = (const uint32_t *)((const uint8_t *)[indirectBuffer contents] + offset);

	pend_binst = a[3];
	[self drawPrimitives:primitiveType vertexStart:a[2] vertexCount:a[0] instanceCount:a[1]];
}''')
s = rep(s, '''indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount
{
	[self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset];
}''', '''indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount
{
	pend_inst = (uint32_t)instanceCount;
	[self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType
		  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount
		   baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance
{
	pend_bvert = (int32_t)baseVertex;
	pend_binst = (uint32_t)baseInstance;
	[self drawIndexedPrimitives:primitiveType indexCount:indexCount indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset
		      instanceCount:instanceCount];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
	    indexBufferOffset:(NSUInteger)indexBufferOffset indirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)offset
{
	/* MTLDrawIndexedPrimitivesIndirectArguments: indexCount, instanceCount, indexStart, baseVertex (i32), baseInstance */
	const uint32_t *a = (const uint32_t *)((const uint8_t *)[indirectBuffer contents] + offset);
	NSUInteger isz = indexType == MTLIndexTypeUInt32 ? 4 : 2;

	[self drawIndexedPrimitives:primitiveType indexCount:a[0] indexType:indexType indexBuffer:indexBuffer indexBufferOffset:indexBufferOffset + a[2] * isz
		      instanceCount:a[1] baseVertex:(int32_t)a[3] baseInstance:a[4]];
}''')

# commit: three words after the last trailing section
s = rep(s, '''			[call appendBytes:stl length:sizeof stl];
''', '''			[call appendBytes:stl length:sizeof stl];
			uint32_t di[3] = { [d[@"inst"] unsignedIntValue] ? [d[@"inst"] unsignedIntValue] : 1u, [d[@"binst"] unsignedIntValue], [d[@"bvert"] unsignedIntValue] };

			[call appendBytes:di length:sizeof di];
''')
open(shim, 'w', newline='\n').write(s)

r = open(srv).read()
r = rep(r, 'let (has_st, sref_f, sref_b, sclear, sload, resolve_id) = {', 'let (has_st, sref_f, sref_b, sclear, sload, resolve_id, ninst, binst, bvert) = {')
r = rep(r, '''                        if p.len() >= o + 24 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20))
                        } else if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), 0)
                        } else {
                            (false, 0, 0, 0, 1, 0)
                        }''', '''                        if p.len() >= o + 36 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20),
                             u32at(&p, o + 24).max(1), u32at(&p, o + 28), u32at(&p, o + 32) as i32)
                        } else if p.len() >= o + 24 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), u32at(&p, o + 20), 1, 0, 0)
                        } else if p.len() >= o + 20 {
                            (u32at(&p, o) != 0, u32at(&p, o + 4), u32at(&p, o + 8), u32at(&p, o + 12), u32at(&p, o + 16), 0, 1, 0, 0)
                        } else {
                            (false, 0, 0, 0, 1, 0, 1, 0, 0)
                        }''')
r = rep(r, 'enc.draw_indexed_primitives(pt, count, it, ib, idxoff, 1, 0, 0)?;', 'enc.draw_indexed_primitives(pt, count, it, ib, idxoff, ninst, bvert, binst)?;')
# the second draw_primitives (op 12, after the indexed branch)
k = r.index('enc.draw_indexed_primitives(pt, count, it, ib, idxoff, ninst, bvert, binst)?;')
j = r.index('enc.draw_primitives(pt, start as u32, count, 1, 0)?;', k)
r = r[:j] + 'enc.draw_primitives(pt, start as u32, count, ninst, binst)?;' + r[j + len('enc.draw_primitives(pt, start as u32, count, 1, 0)?;'):]
open(srv, 'w', newline='\n').write(r)
print('patched')
