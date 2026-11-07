import struct,sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
d=open('hopay2/hofull.payload','rb').read()
nsegs=struct.unpack_from('<I',d,12)[0]
segs=[struct.unpack_from('<QQQQ',d,0x2a8+32*i) for i in range(nsegs)]
def read(va,n):
    for vm,vs,fo,fs in segs:
        if vm<=va<vm+fs: return d[fo+(va-vm):fo+(va-vm)+n]
syms={};order=[]
for l in open('kernel.syms'):
    p=l.split()
    if len(p)==3: syms[int(p[0],16)]=p[2]; order.append(int(p[0],16))
order.sort()
name=sys.argv[1]
start=[a for a,n in syms.items() if n==name][0]
nxt=[a for a in order if a>start][0]
code=read(start,nxt-start)
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False
for i in md.disasm(code,start):
    if i.mnemonic=='bl':
        t=int(i.op_str.lstrip('#'),0); print(hex(i.address),'bl',syms.get(t,hex(t)))
    elif i.mnemonic=='b' and not i.op_str.startswith('#0x'+hex(start)[2:6]):
        t=int(i.op_str.lstrip('#'),0)
        if not (start<=t<nxt): print(hex(i.address),'b(tail)',syms.get(t,hex(t)))
print('size',hex(nxt-start))
