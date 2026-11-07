#!/bin/bash
# One supervised scan experiment. Temporary test app and payload; Android is the exit.
set -u
export PATH=$PATH:/opt/homebrew/bin
pkg=com.cubs.scantest
SCAN_WAIT=${SCAN_WAIT:-120}
case "$SCAN_WAIT" in ''|*[!0-9]*) echo 'invalid scan wait'; exit 1;; esac
[ "$SCAN_WAIT" -le 240 ] || { echo 'scan wait exceeds watchdog budget'; exit 1; }
rootsh() { printf '%s\n' "$1" | adb shell /data/adb/ksu/bin/ksud debug su; }
fail() { echo "FAIL: $*"; exit 1; }
installed=0
armed=0
cleanup() {
  if [ "$armed" = 1 ]; then echo 'recovery pending: phone watchdog remains armed'; fi
  if [ "$installed" = 1 ] && [ "$armed" = 0 ]; then adb uninstall "$pkg" >/dev/null 2>&1 || echo test-uninstall-failed; fi
}
trap cleanup EXIT
[ "$(adb shell getprop sys.boot_completed | tr -d '\r')" = 1 ] || fail android-not-ready
adb shell pm path "$pkg" 2>/dev/null | grep '^package:' >/dev/null && fail test-package-exists
rootsh 'cmp /data/local/ios/ho.payload /data/local/ios/ho.payload.prod || exit 1; for f in boot_ios smp cam snd camid camdelay noring cpu6_khz ios_start.pre-cubsscan; do test ! -e /data/local/ios/$f || exit 1; done; echo preflight-ok' | grep preflight-ok >/dev/null || fail preflight
read -r RAMOFF LISTOFF < /tmp/cubsscan1-offsets
[ "$RAMOFF" = 28c8000 ] && [ "$LISTOFF" = 17fe00000 ] || fail offsets-changed
apk=$(cat /tmp/cubs-scan-test-apk-path) || fail apk-path
adb install "$apk" >/dev/null || fail install
installed=1
adb shell pm grant "$pkg" android.permission.ACCESS_FINE_LOCATION || fail permission
adb pull /data/local/ios/ios_start.sh /tmp/cubsscan-launcher.sh >/dev/null || fail pull-launcher
python3 - <<'PY' || exit 1
p='/tmp/cubsscan-launcher.sh'; s=open(p).read()
a='exec > $L 2>&1'; b='. $D/env.sh'; c='[ -x $D/netsrv ] && sh $D/netsrv.sh &'
assert s.count(a)==s.count(b)==s.count(c)==1
watch='''
( echo "$(date): timer started" >> $D/cubsscan-watchdog.log
  sleep 420
  echo "$(date): watchdog fired" >> $D/cubsscan-watchdog.log
  cp $D/ho.payload.prod $D/ho.payload.cubsscan.tmp && mv -f $D/ho.payload.cubsscan.tmp $D/ho.payload
  cp $D/ios_start.pre-cubsscan $D/ios_start.cubsscan.tmp && chmod 755 $D/ios_start.cubsscan.tmp && mv -f $D/ios_start.cubsscan.tmp $D/ios_start.sh
  rm -f $D/boot_ios $D/smp
  echo "$(date): reboot requested" >> $D/cubsscan-watchdog.log
  sync; reboot
) &'''
s=s.replace(a,a+watch).replace(b,b+'\nRAMOFF=28c8000')
old='[ -f $D/smp ] && echo 0 > /sys/devices/system/cpu/cpu5/online'
assert s.count(old)==1
s=s.replace(old,'''if [ -f $D/smp ]; then
  echo 0 > /sys/devices/system/cpu/cpu4/online
  if [ "$(cat /sys/devices/system/cpu/cpu4/online)" != 0 ]; then echo 'CPU4 offline failed'; exit 1; fi
fi
'''+old)
assert s.count('stop surfaceflinger; stop vendor.hwcomposer-3; sleep 2')==1
s=s.replace('stop surfaceflinger; stop vendor.hwcomposer-3; sleep 2', '''WIFI_APP_UID=$(cmd package list packages -U com.cubs.scantest | sed -n 's/.*uid://p' | tr -d '\\r')
case "$WIFI_APP_UID" in ''|*[!0-9]*) echo 'wifi app uid unavailable'; exit 1;; esac
am start-foreground-service --user 0 -n com.cubs.scantest/com.cubs.cam.WifiStatusService --ez scans true > $D/cubsscan-app.log 2>&1
sleep 2
stop surfaceflinger; stop vendor.hwcomposer-3; sleep 2''')
s=s.replace(c,c+'''
/data/local/tmp/wifisrv "$WIFI_APP_UID" > $D/cubsscan-status.log 2>&1 &
/data/local/tmp/wifiscansrv "$WIFI_APP_UID" > $D/cubsscan-scans.log 2>&1 &''')
open(p,'w').write(s)
PY
adb push "$HOME/pcwork/hopay2/cubsscan1r.payload" /data/local/tmp/cubsscan1.payload >/dev/null || fail push-payload
adb push /tmp/cubsscan-launcher.sh /tmp/wifisrv /tmp/wifiscansrv /data/local/tmp/ >/dev/null || fail push-inputs
if [ "${BINDER_PROBE:-0}" = 1 ]; then
  adb push /tmp/cubs-wifi-binder-probe.dex /data/local/tmp/cubs-wifi-binder-probe.dex >/dev/null || fail push-binder-probe
fi
rootsh 'rm -f /data/local/ios/cubsscan-watchdog.log; cp /data/local/ios/ios_start.sh /data/local/ios/ios_start.pre-cubsscan && cp /data/local/tmp/cubsscan-launcher.sh /data/local/ios/ios_start.cubsscan.tmp && chmod 755 /data/local/ios/ios_start.cubsscan.tmp /data/local/tmp/wifisrv /data/local/tmp/wifiscansrv && mv -f /data/local/ios/ios_start.cubsscan.tmp /data/local/ios/ios_start.sh && cp /data/local/tmp/cubsscan1.payload /data/local/ios/ho.payload && touch /data/local/ios/smp && sync && echo armed' | grep armed >/dev/null || fail arm
armed=1
echo 'starting iOS scan experiment'
rootsh 'sh /data/local/ios/ios_next.sh' || :
ready=0
for i in $(seq 1 150); do
  if adb shell "grep -q 'timer started' /data/local/ios/cubsscan-watchdog.log && grep -q 'XNU started' /data/local/ios/ios_start.log" 2>/dev/null; then ready=1; break; fi
  sleep 2
done
if [ "$ready" = 1 ]; then
  echo 'iOS started; collecting scan evidence'
  if [ "${BINDER_PROBE:-0}" = 1 ]; then
    rootsh 'CLASSPATH=/data/local/tmp/cubs-wifi-binder-probe.dex timeout 10 /system/bin/app_process /system/bin com.cubs.probe.WifiBinderProbe' || echo binder-probe-command-failed
  fi
  sleep "$SCAN_WAIT"
  rootsh 'insmod /data/local/tmp/cubs_log_reader.ko && head -c 50331648 /dev/cubs_log > /data/local/tmp/cubsscan-log.bin && rmmod cubs_log_reader && echo log-read-ok' || :
  adb pull /data/local/tmp/cubsscan-log.bin /tmp/cubsscan-log.bin >/dev/null 2>&1 || :
  rootsh '/data/local/tmp/ringrd 0x1C00000 0x400000 /data/local/tmp/cubsscan-shot.bin' || :
  adb pull /data/local/tmp/cubsscan-shot.bin /tmp/cubsscan-shot.bin >/dev/null 2>&1 || :
else
  echo 'iOS startup not observed'
fi
echo 'restoring Android'
rootsh 'cp /data/local/ios/ho.payload.prod /data/local/ios/ho.payload.cubsscan.tmp && mv -f /data/local/ios/ho.payload.cubsscan.tmp /data/local/ios/ho.payload; cp /data/local/ios/ios_start.pre-cubsscan /data/local/ios/ios_start.cubsscan.tmp && chmod 755 /data/local/ios/ios_start.cubsscan.tmp && mv -f /data/local/ios/ios_start.cubsscan.tmp /data/local/ios/ios_start.sh; rm -f /data/local/ios/boot_ios /data/local/ios/smp; sync; reboot' || :
perl -e 'alarm 180; exec @ARGV' adb wait-for-device || fail reconnect
ready=0
for i in $(seq 1 70); do
  if [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && adb shell '! pidof drmfb2 >/dev/null' 2>/dev/null; then ready=1; break; fi
  sleep 3
done
[ "$ready" = 1 ] || fail android-not-ready-after-reboot
rootsh 'cmp /data/local/ios/ho.payload /data/local/ios/ho.payload.prod && cmp /data/local/ios/ios_start.sh /data/local/ios/ios_start.pre-cubsscan && test ! -e /data/local/ios/boot_ios && test ! -e /data/local/ios/smp && echo recovery-verified' | grep recovery-verified || fail recovery-check
armed=0
rootsh 'rm -f /data/local/ios/ios_start.pre-cubsscan /data/local/tmp/cubsscan-log.bin /data/local/tmp/cubsscan-shot.bin /data/local/tmp/cubsscan-launcher.sh; sync'
echo test-complete
