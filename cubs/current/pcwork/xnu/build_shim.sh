#!/bin/bash
# Run on the Mac: build libmtlshim.dylib and the Metal test programs for iOS. Sources are expected in $HOME.
cd ~/unixnu-pixel11/hackios-upstream
export THEOS=$HOME/unixnu-pixel11/theos
source xnu-env.sh
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
cd ~
xnu-clang -arch arm64 -isysroot $SDK -fno-objc-arc -O1 -Wno-incomplete-implementation -Wno-protocol -Wno-objc-property-implementation -Wno-deprecated-declarations \
	-dynamiclib -install_name /redwood/bin/libmtlshim.dylib -framework Foundation -framework QuartzCore -framework CoreGraphics -lobjc -o libmtlshim.dylib mtlshim.m 2>&1 | grep -E "error" | head -10
for app in metal_hello metal_triangle metal_present; do
	xnu-clang -arch arm64 -isysroot $SDK -fno-objc-arc -O1 -Wno-deprecated-declarations -I$HOME -o $app $app.m libmtlshim.dylib \
		-framework Foundation -lobjc 2>&1 | grep -E "error" | head -10
done
ls -l libmtlshim.dylib metal_hello metal_triangle metal_present
