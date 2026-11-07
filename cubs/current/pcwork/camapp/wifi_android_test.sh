#!/system/bin/sh
uid=$(cmd package list packages -U com.cubs.cam | sed -n 's/.*uid://p' | tr -d '\r')
case "$uid" in ''|*[!0-9]*) echo 'app UID unavailable'; exit 1;; esac
rm -f /data/local/tmp/wifi-test-ring /data/local/tmp/wifi-test.log
touch /data/local/tmp/wifi-test-ring
CUBS_WIFI_RING=/data/local/tmp/wifi-test-ring /data/local/tmp/wifisrv "$uid" > /data/local/tmp/wifi-test.log 2>&1 &
pid=$!
cleanup() { am stopservice --user 0 -n com.cubs.cam/com.cubs.cam.WifiStatusService >/dev/null 2>&1; kill "$pid" 2>/dev/null; rm -f /data/local/tmp/wifi-test-ring; }
trap cleanup EXIT
sleep 1
am start-foreground-service --user 0 -n com.cubs.cam/com.cubs.cam.WifiStatusService >/dev/null
sleep 15
cat /data/local/tmp/wifi-test.log
