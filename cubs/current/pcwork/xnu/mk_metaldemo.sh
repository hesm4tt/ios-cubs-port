#!/bin/bash
# Run on the Mac (metaldemo.m, fixture_bc.h in ~, libmtlshim.dylib in ~): builds MetalDemo.app into the userland tree's ../apps,
# which the system image build (SYSTEM=1 bash build.sh) copies into /Applications.
export PATH=$PATH:/opt/homebrew/bin
cd ~/unixnu-pixel11/hackios-upstream
source xnu-env.sh
export THEOS=$HOME/unixnu-pixel11/theos
APP=$HOME/unixnu-pixel11/hackios-upstream/apps/MetalDemo.app
rm -rf $APP
mkdir -p $APP
cd ~
xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -fno-objc-arc -O1 -Wno-deprecated-declarations -I$HOME -o $APP/MetalDemo metaldemo.m \
	libmtlshim.dylib -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics -lobjc 2>&1 | grep -E "error" | head
cp ~/metaldemo.plist $APP/Info.plist
printf 'APPL????' > $APP/PkgInfo
python3 - $APP <<'PY'
import struct, sys, zlib
out = sys.argv[1]
for size, scale in ((120, 2), (180, 3)):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            inside = (2 * x + 1) * size < (2 * y + 1) * size
            rows += bytes((255, 180, 30) if inside else (20, 110, 40))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
PY
ls -l $APP
