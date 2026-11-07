#!/bin/bash
# usage: ab_test.sh   (on the Mac) runs the fast and the plain blit back to back, reporting cost cookies
export PATH=$PATH:/opt/homebrew/bin
cd ~/pcwork
python3 - <<PY
import struct
d=bytearray(open('hopay2/fast.payload','rb').read())
for name,extra in (('fastA',b''),('slowA',b' cubs_blit_fast=0')):
    n=b'debug=0x8 keepsyms=1 cpus=1 rd=md0 loader_return_after=200 cubs_blit=3 cubs_touch=1'+extra
    e=bytearray(d); e[0x48:0x48+608]=n+b'\0'*(608-len(n)); open('hopay2/%s.payload'%name,'wb').write(e)
hdr=struct.unpack_from('<IIQQQQQQQ',d,8); virt,kend,mem=hdr[2],hdr[6],hdr[8]
print('RAMOFF',hex((kend-virt)-((2492203008+0x3fff)&~0x3fff)),'LISTOFF',hex(mem-0x100000))
PY
for v in fastA slowA; do
  for i in $(seq 1 60); do s=$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d "\r"); [ "$s" = 1 ] && break; sleep 10; done
  adb shell "pkill drmfb; start surfaceflinger; start vendor.hwcomposer-3"; sleep 8
  echo "== $v"
  python3 pcrun.py full --payload hopay2/$v.payload --ramoff 28a8000 --drm 17fe00000 --q "ost 0x" --cmd K --wait 500 2>&1 | grep -E "android back|Reboot reason|seconds until" | tail -3
done
