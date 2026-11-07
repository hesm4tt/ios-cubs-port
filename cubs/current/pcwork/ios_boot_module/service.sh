#!/system/bin/sh
# Dual boot hook: runs at every Android boot (KernelSU module service). Android stays the default OS.
D=/data/local/ios
while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done
sleep 10
[ -f $D/ios_start.sh ] || exit 0
if [ -f $D/boot_ios ]; then
	rm -f $D/boot_ios
	echo "flag file: starting iOS at $(date)" >> $D/boot_hook.log
	sh $D/ios_start.sh &
	exit 0
fi
# three volume-down presses within 60 s of the boot finishing
n=$(timeout 60 getevent -l /dev/input/event0 2>/dev/null | awk '/KEY_VOLUMEDOWN +DOWN/ { n++; if (n >= 3) { print n; exit } }')
if [ "$n" = 3 ]; then
	echo "volume-down x3: starting iOS at $(date)" >> $D/boot_hook.log
	sh $D/ios_start.sh &
fi
