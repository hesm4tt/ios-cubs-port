#!/bin/bash
set -e
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
clang --target=aarch64-linux-gnu -c blobs.S -o blobs.o
llvm-objcopy -O binary --only-section=.text blobs.o blobs.bin
python3 - <<PY
import struct
d=open("blobs.bin","rb").read()
import subprocess
nm=subprocess.run(["llvm-nm","blobs.o"],capture_output=True,text=True).stdout
sym={l.split()[2]:int(l.split()[0],16) for l in nm.strip().split("\n")}
ss,se,ts,te=sym["stub_start"],sym["stub_end"],sym["tramp_start"],sym["tramp_end"]
def arr(name,b):
    w=struct.unpack("<%dI"%(len(b)//4),b)
    return "static const u32 %s[] = {\n\t%s\n};\n#define %s_WORDS %d\n"%(name,",\n\t".join("0x%08x"%x for x in w),name.upper(),len(w))
open("blobs.h","w").write(arr("stub_code",d[ss:se])+arr("tramp_code",d[ts:te]))
print(ss,se,ts,te)
PY
cat blobs.h | head -50
