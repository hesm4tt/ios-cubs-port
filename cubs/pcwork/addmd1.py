"""Append the 4 MiB root.apfs as /dev/md1 (ADT RAMDisk1) to a hand-over payload.
usage: python addmd1.py in.payload root.apfs out.payload"""
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'references', 'ios-bluejay', 'tools'))
import adt
PAGE = 0x4000; SEG_TABLE = 0x2a8
src, rootimg, dst = sys.argv[1:4]
d = open(src, 'rb').read()
ns = struct.unpack_from('<I', d, 12)[0]
vb, ent, ao, asz, ke, pb, ms = struct.unpack_from('<7Q', d, 0x10)
segs = []
for i in range(ns):
    va, vs, fo, fs = struct.unpack_from('<QQQQ', d, SEG_TABLE + 32 * i)
    segs.append((va, vs, d[fo:fo + fs]))
blob = d[ao:ao + asz]
img = open(rootimg, 'rb').read()
size = (len(img) + PAGE - 1) & ~(PAGE - 1)
rd_va = ke; ke2 = rd_va + size; rd_pa = rd_va - vb + pb
root, _ = adt.parse(blob)
chosen = next(c for c in root.children if c.name == 'chosen')
mmap = next(c for c in chosen.children if c.name == 'memory-map')
assert mmap.prop('RAMDisk1') is None
mmap.props.append(('RAMDisk1', 0, struct.pack('<2Q', rd_pa, size)))
blob2 = adt.serialize(root)
segs.append((rd_va, size, img))
out = bytearray(SEG_TABLE + 32 * len(segs))
tab = []
for va, vs, data in segs:
    off = len(out); out += data; out += b'\0' * (-len(out) & 0xf); tab.append((va, vs, off, len(data)))
adt_off = len(out); out += blob2; out += b'\0' * (-len(out) & 0xf)
out[0:0x48] = d[0:0x48]
out[0x48:SEG_TABLE] = d[0x48:SEG_TABLE]
struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, len(segs))
struct.pack_into('<7Q', out, 0x10, vb, ent, adt_off, len(blob2), ke2, pb, ms)
for i, t in enumerate(tab): struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)
open(dst, 'wb').write(out)
print('md1 at PA', hex(rd_pa), 'size', hex(size), 'kernel_end', hex(ke2), 'payload', len(out))
