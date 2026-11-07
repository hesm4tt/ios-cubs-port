#!/system/bin/sh
set -u
pkg=com.cubs.scantest
uid=$(cmd package list packages -U "$pkg" | sed -n 's/.*uid://p' | tr -d '\r')
case "$uid" in ''|*[!0-9]*) echo uid-unavailable; exit 1;; esac
ring=$(mktemp /data/local/tmp/cubs-real-scan.XXXXXX) || exit 1
server=
statusserver=
cleanup() {
  am stopservice --user 0 -n "$pkg/com.cubs.cam.WifiStatusService" >/dev/null 2>&1 || :
  [ -z "$server" ] || kill "$server" 2>/dev/null || :
  [ -z "$statusserver" ] || kill "$statusserver" 2>/dev/null || :
  rm -f "$ring" /data/local/tmp/cubs-scan-status-test.log
}
trap cleanup EXIT
am start-foreground-service --user 0 -n "$pkg/com.cubs.cam.WifiStatusService" --ez scans true >/dev/null || exit 1
sleep 3
CUBS_WIFI_RING="$ring" /data/local/tmp/wifiscansrv "$uid" &
server=$!
CUBS_WIFI_RING="$ring" /data/local/tmp/wifisrv "$uid" > /data/local/tmp/cubs-scan-status-test.log 2>&1 &
statusserver=$!
sleep 1
kill -0 "$server" || exit 1
kill -0 "$statusserver" || exit 1
sleep 12
echo "bridge header $(dd if="$ring" bs=1 skip=$((0x2100000)) count=12 2>/dev/null | od -An -tu4)"
echo "snapshot header $(dd if="$ring" bs=1 skip=$((0x2100100)) count=16 2>/dev/null | od -An -tu4)"
echo "status powered-connected $(dd if="$ring" bs=1 skip=$((0x2000008)) count=8 2>/dev/null | od -An -tu4)"
echo "status security-elements $(dd if="$ring" bs=1 skip=$((0x200001c)) count=8 2>/dev/null | od -An -td4)"
