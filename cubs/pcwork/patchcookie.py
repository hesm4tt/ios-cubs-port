import struct,sys,subprocess
# usage: patchcookie.py <symbol> <tag> ; reads ~/hopay/ho.payload, writes ~/hopay/ho_patched.payload
import os
sym,tag=sys.argv[1],int(sys.argv[2],0)
K=os.path.expanduser("~/unixnu-pixel11/hackios-upstream/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837")
nm=subprocess.run(["/opt/homebrew/opt/llvm/bin/llvm-nm",K],capture_output=True,text=True).stdout
va=None
for l in nm.split("\n"):
    p=l.split()
    if len(p)==3 and p[2]==sym: va=int(p[0],16)
assert va, sym
d=bytearray(open(os.path.expanduser("~/hopay/"+(sys.argv[3] if len(sys.argv)>3 else "ho.payload")),"rb").read())
nsegs=struct.unpack_from("<I",d,12)[0]
off=None
for i in range(nsegs):
    vm,vs,fo,fs=struct.unpack_from("<QQQQ",d,0x2a8+32*i)
    if vm<=va<vm+fs: off=fo+(va-vm); break
assert off, "va not in a file-backed segment"
cookie=0xc0000000|tag
lo,hi=cookie&0xffff,cookie>>16
words=[0xd2800240,0xf2b08000,0xd2800021,0xf2b00001,0xd2800002|(lo<<5),0xf2a00002|(hi<<5),0xd4000003,0x14000000]
for k,w in enumerate(words): struct.pack_into("<I",d,off+4*k,w)
open(os.path.expanduser("~/hopay/ho_patched.payload"),"wb").write(d)
print(sym,hex(va),"file off",hex(off),"cookie",hex(cookie))
