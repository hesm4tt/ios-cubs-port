#!/system/bin/sh
# Android-only lifecycle test. No iOS boot and no writes to reserved memory.
set -u
cleanup() {
  if [ -n "${holder:-}" ]; then kill "$holder" 2>/dev/null || :; wait "$holder" 2>/dev/null || :; fi
  if grep -q '^cubs_log_reader ' /proc/modules; then rmmod cubs_log_reader || echo unload-failed; fi
}
trap cleanup EXIT HUP INT TERM
if grep -q '^cubs_log_reader ' /proc/modules; then echo already-loaded; exit 1; fi
insmod /data/local/tmp/cubs_log_reader.ko || { echo load-failed; exit 1; }
echo load-ok
ls -lZ /dev/cubs_log
if ! head -c 4096 /dev/cubs_log >/dev/null; then echo read-failed; exit 1; fi
echo read-ok
( exec 3</dev/cubs_log; sleep 20 ) &
holder=$!
sleep 1
if rmmod cubs_log_reader 2>/dev/null; then echo unload-while-open-unexpected; exit 1; fi
echo held-open-unload-refused
kill "$holder" 2>/dev/null || :
wait "$holder" 2>/dev/null || :
holder=
rmmod cubs_log_reader || { echo unload-failed; exit 1; }
echo unload-ok
