#!/bin/bash
# Run on the Mac (sources in ~/glfwd, gl.xml in ~): builds libredwood-gl.dylib (iOS, GL ES forwarding) and glsrv (Android)
export PATH=$PATH:/opt/homebrew/bin
cd ~/unixnu-pixel11/hackios-upstream
source xnu-env.sh
export THEOS=$HOME/unixnu-pixel11/theos
cd userland
DYN="xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1"
mkdir -p obj
$DYN -c -Isrc -I$HOME/glfwd -o /tmp/glfwd.o $HOME/glfwd/gl_fwd.c 2>&1 | grep -E "error|warning: impl" | head -20
nm -gU /tmp/glfwd.o | awk '$2=="T"{sub(/^_/,"",$3);print $3}' | grep '^gl' > $HOME/glfwd/exported-gl.txt
wc -l < $HOME/glfwd/exported-gl.txt
./mkeagl.py src/eagl_slots.txt $HOME/gl.xml $HOME/glfwd/exported-gl.txt src/eagl.c > obj/eagl_gen.h 2> /tmp/mkeagl.err; head -5 /tmp/mkeagl.err
$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -Isrc -I$HOME/glfwd -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc -o $HOME/libredwood-gl.dylib src/eagl.c $HOME/glfwd/gl_fwd.c 2>&1 | grep -E "error|undefined" | head -20
ls -l $HOME/libredwood-gl.dylib
N=/opt/homebrew/share/android-ndk; T=$N/toolchains/llvm/prebuilt/darwin-x86_64
$T/bin/aarch64-linux-android31-clang -O2 -I$HOME/glfwd -o $HOME/glsrv $HOME/glfwd/glsrv.c -lEGL -lGLESv3 2>&1 | grep -E "error|warning: impl" | head
ls -l $HOME/glsrv
