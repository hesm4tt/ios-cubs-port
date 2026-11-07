#!/system/bin/sh
set -u
ring=$(mktemp /data/local/tmp/cubs-request-test.XXXXXX) || exit 1
server=; client=
cleanup() { [ -z "$client" ] || kill "$client" 2>/dev/null || :; [ -z "$server" ] || kill "$server" 2>/dev/null || :; rm -f "$ring"; }
trap cleanup EXIT
CUBS_WIFI_RING="$ring" /data/local/tmp/wifiscansrv 10000 &
server=$!
sleep 1
/data/local/tmp/wifiscansrv_test 10000 request &
client=$!
sleep 1
printf '\001\000\000\000' | dd of="$ring" bs=1 seek=$((0x2100080)) conv=notrunc 2>/dev/null
wait "$client"
client=
