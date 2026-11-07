#!/system/bin/sh
# Start iOS (XNU on CPU6) beside the running Android and leave it running: the "boot iOS" half of the dual boot.
# Android is the default OS; this is run (as root) on request, by the boot hook when the volume-down key is held at power-on or a flag file asks for it
# (pcwork/ios_boot_module), or by hand: `sh /data/local/ios/ios_start.sh`.
# Back to Android: hold power + volume down for 3 s on iOS (XNU writes BYE1 into the shared page, the watcher below reboots), or `reboot` from adb.
# Files (all under /data/local/ios): ho.payload (bootreg'd XNU payload), cubs_stage.ko, drmfb2, mtlsrv, glsrv, system.img (the iOS system image),
# env.sh (RAMOFF=.. LISTOFF=.. exported).
D=/data/local/ios
L=$D/ios_start.log
exec > $L 2>&1
. $D/env.sh
cd $D
rm -f tri.ppm
rmmod google_ehld_coreinstr 2>/dev/null
for d in e500000.pinctrl da2c000.spi; do echo on > /sys/devices/platform/$d/power/control; done
# the XNU core (CPU6) runs at a moderate clock
fq=/sys/devices/system/cpu/cpufreq/policy6
KHZ=3033000; [ -f $D/cpu6_khz ] && KHZ=$(cat $D/cpu6_khz)
echo $KHZ > $fq/scaling_max_freq; echo $KHZ > $fq/scaling_min_freq
# thermal guard: log every 30 s, reboot into Android (clean) if the battery passes 43 C or BIG 80 C
( while true; do b=$(cat /sys/class/power_supply/battery/temp); big=$(for z in /sys/class/thermal/thermal_zone*; do [ "$(cat $z/type)" = BIG ] && cat $z/temp; done); echo "temp $(date +%T) batt=$b big=$big cpu6=$(cat $fq/scaling_cur_freq)" >> $D/temps.log; if [ "$b" -gt 430 ] 2>/dev/null || [ "${big:-0}" -gt 80000 ] 2>/dev/null; then echo "THERMAL STOP" >> $D/temps.log; sync; reboot; fi; sleep 30; done ) &
input keyevent KEYCODE_WAKEUP; svc power stayon true
# the Android camera for iOS (flag file /data/local/ios/cam; app com.cubs.cam + camsrv, see pcwork/camapp): the app is started while the display is still up
if [ -f $D/cam ]; then
	am start-foreground-service --user 0 -n com.cubs.cam/com.cubs.cam.CamService --es id $(cat $D/camid 2>/dev/null || echo 0) --ei w 640 --ei h 480 --ei delay $(cat $D/camdelay 2>/dev/null || echo 0) > $D/camapp.log 2>&1
	sleep 6
fi
# the sound of iOS (flag file /data/local/ios/snd): the app plays what sndsrv takes out of the ring
if [ -f $D/snd ]; then
	am start-foreground-service --user 0 -n com.cubs.cam/com.cubs.cam.SndService > $D/sndapp.log 2>&1
	sleep 3
fi
stop surfaceflinger; stop vendor.hwcomposer-3; sleep 2
( ./drmfb2 $D/fblist.bin hold > drmfb.log 2>&1 & )
sleep 6
cat drmfb.log
rmmod cubs_stage 2>/dev/null; rm -f /dev/cubs_stage /dev/cubs_go /dev/cubs_ram
insmod $D/cubs_stage.ko; sleep 2
cat $D/ho.payload > /dev/cubs_stage
taskset 1 sh -c "echo S > /dev/cubs_go"
printf O$RAMOFF > /dev/cubs_go
cat $D/system.img > /dev/cubs_ram
printf O$LISTOFF > /dev/cubs_go
cat $D/fblist.bin > /dev/cubs_ram
printf L$LISTOFF:2000 > /dev/cubs_go
printf "L0:97168000" > /dev/cubs_go
printf "L99100000:100000" > /dev/cubs_go
printf "F922002900:0:100" > /dev/cubs_go
echo C > /dev/cubs_go
[ -f $D/smp ] && echo 0 > /sys/devices/system/cpu/cpu5/online
[ -f $D/smp ] && { echo 2500000 > /sys/devices/system/cpu/cpufreq/policy2/scaling_max_freq; echo 2500000 > /sys/devices/system/cpu/cpufreq/policy2/scaling_min_freq; }
echo 0 > /sys/devices/system/cpu/cpu6/online; sleep 1
printf "N600:1" > /dev/cubs_go
echo "XNU started on CPU6 at $(date)"
# the GPU servers' ring (RAM that XNU does not manage); clear the return-to-Android word first
printf "Eab2000000:10000000" > /dev/cubs_go
printf "O0" > /dev/cubs_go
dd if=/dev/zero of=/dev/cubs_ram bs=4096 seek=$((0xFF0)) count=1 2>/dev/null
export HOME=$D XDG_CACHE_HOME=$D/xdg
RING_DEV=/dev/cubs_ram RING_BASE=0 ./mtlsrv > gserver.log 2>&1 &
RING_DEV=/dev/cubs_ram RING_BASE=0 ./glsrv > glsrv.log 2>&1 &
[ -x $D/netsrv ] && sh $D/netsrv.sh &
[ -x $D/battsrv ] && ( $D/battsrv > $D/battsrv.log 2>&1 & )
[ -f $D/snd ] && ( $D/sndsrv > $D/sndsrv.log 2>&1 & )
if [ -f $D/cam ]; then if [ -f $D/noring ]; then ( CAMSRV_SAVE=0 $D/camsrv > $D/camsrv.log 2>&1 & ); else ( CAMSRV_RING=1 CAMSRV_SAVE=0 $D/camsrv > $D/camsrv.log 2>&1 & ); fi; fi
# watcher: "BYE1" in the shared page (XNU: power + volume down held) means reboot into Android
while true; do
	v=$(dd if=/dev/cubs_ram bs=4 skip=$((0xFF0000 / 4)) count=1 2>/dev/null | od -An -tx4 | tr -d ' ')
	if [ "$v" = 31455942 ]; then
		echo "BYE1 seen at $(date): rebooting into Android"
		sync
		reboot
	fi
	sleep 1
done
