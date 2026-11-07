#!/bin/bash
# usage: mac_build.sh OUT.payload "CMDLINE"
set -e
D=~/unixnu-pixel11; B=$D/cubs-xnu-bootstrap; P=/opt/homebrew/bin/python3
S=$D/hackios-upstream/xnu-src/xnu-8796.121.2
cp ~/loader_return.c.bak-blit $S/osfmk/arm64/loader_return.c
cat ~/cubs_blit.c.inc ~/cubs_gpu_tab.h ~/cubs_gpu.c.inc ~/cubs_touch.c.inc >> $S/osfmk/arm64/loader_return.c
cd $D/hackios-upstream
bash xnu-build.sh > ~/xnu_build_blit.log 2>&1 || true
K=$S/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l $K
cd ~/hopay
$P $B/ios-cubs-port/references/ios-bluejay/tools/mkpayload.py $K ${ADT:-~/hopay/ho.adt} ~/hopay/$1 --cmdline "$2" --ramdisk1 ${ROOT:-~/hopay/root.apfs} --ramdisk-reserve 2492203008 2>&1 | tail -1
ls -l ~/hopay/$1
