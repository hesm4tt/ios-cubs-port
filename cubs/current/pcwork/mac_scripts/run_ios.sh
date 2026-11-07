#!/bin/bash
# usage: [OVERLAY=1|proc] run_ios.sh NAME [WAIT] ["EXTRA JOB LINES"] ["EXTRA files"]
# The standard iOS run: fast SpringBoard (MobileGestalt stub + timeout patch), OpenGL ES forwarded to the phone's GPU (gl=1).
#   OVERLAY=1     a Metal triangle drawn inside SpringBoard through the shim (libmtloverlay.dylib)
#   OVERLAY=proc  a separate process (metalproc) draws with Metal into a CAContext of its own; SpringBoard shows it with a
#                 CALayerHost (libmtlhost.dylib)
export IOS_EXTRA="${IOS_EXTRA:-com.apple.configd com.apple.mDNSResponder com.apple.mDNSResponderHelper com.apple.trustd com.apple.mediaserverd com.apple.nanoregistryd com.apple.nanoregistrylaunchd com.apple.PosterBoard}"
export JOB_PROGRAM="com.apple.mobilegestalt.xpc=/redwood/bin/mghelper $EXTRA_JP"
printf "%s\n" "/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system" "/redwood/lib/libredwood-sb.dylib SpringBoard" "/redwood/lib/libredwood-media.dylib mediaserverd" "/redwood/lib/libredwood-app.dylib Sileo" "/redwood/bin/libmgpatch.dylib lsd SpringBoard" "/redwood/bin/libmgcam.dylib lsd SpringBoard Camera" > ~/inject_all.txt
FILES="ps_loop mghelper libmgpatch.dylib libmgcam.dylib $EXTRA_FILES2"
JOBS="/redwood/bin/ps_loop 100 3 &;/redwood/bin/net_test wait 45 dhcp en0 waitaddr en0 60"
if [ "$OVERLAY" = 1 ]; then
	echo "/redwood/bin/libmtloverlay.dylib SpringBoard" >> ~/inject_all.txt
	FILES="$FILES libmtlshim.dylib libmtloverlay.dylib"
elif [ "$OVERLAY" = app ]; then
	# MetalDemo.app (pcwork/xnu/metaldemo.m, in the system image since SYSTEM=1 bash build.sh with it in ../apps), opened the
	# way a link opens one, after the lock screen has been swiped away
	FILES="$FILES libmtlshim.dylib"
	JOBS="$JOBS;/redwood/bin/rwtouch wait 100 swipe 0.5 0.985 0.5 0.3 250 wait 6;/redwood/bin/battery_test open metaldemo://"
elif [ "$OVERLAY" = proc ]; then
	echo "/redwood/bin/libmtlhost.dylib SpringBoard" >> ~/inject_all.txt
	FILES="$FILES libmtlshim.dylib metalproc libmtlhost.dylib"
	JOBS="$JOBS;/redwood/bin/metalproc &"
fi
export EXTRA_LIB=$HOME/libredwood-gl.dylib CMDEXTRA="gl=1 cubs_net=1"
[ -n "$CAM" ] && export CMDEXTRA="$CMDEXTRA cam=5 cam_fake=1 cam_fake_fps=15 cam_bridge=1"
bash ~/build_and_run.sh "$1" "$JOBS${3:+;$3}" "$FILES $4" "${2:-330}"
