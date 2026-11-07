#!/system/bin/sh
set -u
pkg=com.cubs.scantest
uid=$(cmd package list packages -U "$pkg" | sed -n 's/.*uid://p' | tr -d '\r')
case "$uid" in ''|*[!0-9]*) echo uid-unavailable; exit 1;; esac
ring=$(mktemp /data/local/tmp/cubs-real-request.XXXXXX) || exit 1
server=
cleanup() {
  am stopservice --user 0 -n "$pkg/com.cubs.cam.WifiStatusService" >/dev/null 2>&1 || :
  [ -z "$server" ] || kill "$server" 2>/dev/null || :
  rm -f "$ring"
}
trap cleanup EXIT
header() { dd if="$ring" bs=1 skip=$((0x2100000)) count=12 2>/dev/null | od -An -tu4; }
snapshot() { dd if="$ring" bs=1 skip=$((0x2100100)) count=16 2>/dev/null | od -An -tu4; }
CUBS_WIFI_RING="$ring" /data/local/tmp/wifiscansrv "$uid" &
server=$!
sleep 1
kill -0 "$server" || exit 1
am start-foreground-service --user 0 -n "$pkg/com.cubs.cam.WifiStatusService" --ez scans true >/dev/null || exit 1
sleep 35
echo "before request bridge $(header) snapshot $(snapshot)"
printf '\001\000\000\000' | dd of="$ring" bs=1 seek=$((0x2100080)) conv=notrunc 2>/dev/null
sleep 12
echo "after request bridge $(header) snapshot $(snapshot)"
echo "first capability $(dd if="$ring" bs=1 skip=$((0x210012a)) count=2 2>/dev/null | od -An -tu2)"
