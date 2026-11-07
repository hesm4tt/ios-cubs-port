#!/bin/bash
# usage: cycle.sh NAME "JOBS" "BOOTARGS" WAIT_SECONDS "GREP_EXTENDED_PATTERN"
# One experiment: build, deploy, boot iOS, wait, read the XNU log (only matching lines), put the ring back, return to Android with the production payload.
export PATH=$PATH:/opt/homebrew/bin
# The current dump switches the global /dev/cubs_ram mapping while bridge
# processes and the BYE watcher still use it.  Refuse to launch an experiment
# until the dump uses an independent mapping or writers are safely quiesced.
echo 'cycle.sh disabled: live log dump changes the shared bridge mapping' >&2
exit 1
NAME=$1; JOBS=$2; BA=$3; WAIT=${4:-420}; PAT=${5:-zzzz}
cd ~
sed "s/CMDEXTRA=\"gl=1 cubs_net=1\"/CMDEXTRA=\"gl=1 cubs_net=1 cubs_bye_after=$((WAIT+150)) $BA\"/" run_ios.sh > run_ios_cyc.sh
NOLAUNCH=1 bash run_ios_cyc.sh $NAME 0 "$JOBS" 2>&1 | tail -1
if [ "$(grep -c error ~/xnu_build_blit.log)" != 0 ]; then grep -m6 error ~/xnu_build_blit.log | cut -c1-220; echo "BUILD FAILED"; exit 1; fi
for i in $(seq 1 40); do adb shell getprop sys.boot_completed 2>/dev/null | grep -q 1 && break; sleep 5; done
adb push ~/pcwork/hopay2/${NAME}r.payload /data/local/tmp/cyc.payload >/dev/null
adb shell "echo '[ -n \"$SMP\" ] && touch /data/local/ios/smp || rm -f /data/local/ios/smp' | /data/adb/ksu/bin/ksud debug su"
adb shell "echo 'cp /data/local/tmp/cyc.payload /data/local/ios/ho.payload; sh /data/local/ios/ios_next.sh' | /data/adb/ksu/bin/ksud debug su"
sleep $WAIT
adb shell "echo 'printf E922000000:30000000 > /dev/cubs_go; printf O0 > /dev/cubs_go; dd if=/dev/cubs_ram of=/data/local/tmp/live.bin bs=1M count=${DUMPMB:-48} 2>/dev/null; printf Eab2000000:10000000 > /dev/cubs_go; printf O0 > /dev/cubs_go' | /data/adb/ksu/bin/ksud debug su"
adb pull /data/local/tmp/live.bin /tmp/live.bin >/dev/null
[ -n "$DUMPMB" ] && { python3 - <<'EOP'
d=open('/tmp/live.bin','rb').read()
i=d.rfind(b'RWCAM001')
print('RWCAM001 at', i)
if i>=0:
    open('/tmp/cam_pic.bin','wb').write(d[i:i+8192+1000000])
EOP
}
if [ -n "" ]; then
PA=
echo "picture PA "
if [ -n "" ]; then
adb shell "echo 'printf E:200000 > /dev/cubs_go; printf O0 > /dev/cubs_go; dd if=/dev/cubs_ram of=/data/local/tmp/pic.bin bs=1M count=2 2>/dev/null; printf Eab2000000:10000000 > /dev/cubs_go; printf O0 > /dev/cubs_go' | /data/adb/ksu/bin/ksud debug su"
adb pull /data/local/tmp/pic.bin /tmp/cam_pic.bin >/dev/null
fi
fi
if [ -n "$FBGRAB" ]; then
FB=$(strings -n 5 /tmp/live.bin | grep -a "CUBS blit: fb pa" | tail -1 | sed -E "s/.*fb pa 0x([0-9a-f]+).*/\1/")
echo "fb pa $FB"
if [ -n "$FB" ]; then
adb push ~/fbgrab.sh /data/local/tmp/fbgrab.sh >/dev/null
adb shell "echo 'sh /data/local/tmp/fbgrab.sh $FB' | /data/adb/ksu/bin/ksud debug su"
adb pull /data/local/tmp/fb.bin /tmp/fb.bin >/dev/null
fi
fi
if [ -n "$SHOTGRAB" ]; then adb shell "echo 'printf Eab2000000:10000000 > /dev/cubs_go; printf O0 > /dev/cubs_go; /data/local/tmp/ringrd 0x1C00000 0x400000 /data/local/tmp/shot.bin' | /data/adb/ksu/bin/ksud debug su"; adb pull /data/local/tmp/shot.bin /tmp/shot.bin >/dev/null; fi
echo "--- log"; strings -n 5 /tmp/live.bin | grep -a -E "$PAT" | cut -c1-230 | tail -${TAILN:-25}
if [ -n "$POST" ]; then echo "--- post"; printf '%s
' "$POST" > /tmp/post.sh; adb push /tmp/post.sh /data/local/tmp/post.sh >/dev/null; adb shell "echo 'sh /data/local/tmp/post.sh' | /data/adb/ksu/bin/ksud debug su" 2>&1 | cut -c1-170 | head -40; fi
echo "--- netsrv/temps"; adb shell "tail -n 1 /data/local/ios/netsrv.log | cut -c1-150; tail -n 1 /data/local/ios/temps.log"
adb shell "echo 'rm -f /data/local/ios/smp; cp /data/local/ios/ho.payload.prod /data/local/ios/ho.payload; sync; reboot' | /data/adb/ksu/bin/ksud debug su"
echo "--- phone returning to Android"
