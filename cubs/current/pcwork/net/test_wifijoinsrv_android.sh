#!/system/bin/sh
set -u
ring=$(mktemp /data/local/tmp/cubs-join-test.XXXXXX) || exit 1
server=
cleanup() { [ -z "$server" ] || kill "$server" 2>/dev/null || :; rm -f "$ring"; }
trap cleanup EXIT
CUBS_JOIN_RING="$ring" /data/local/tmp/wifijoinsrv &
server=$!
sleep 1
kill -0 "$server" || exit 1
printf 'BAD!\001\000\000\000' | dd of="$ring" bs=1 seek=$((0x2200200)) conv=notrunc 2>/dev/null
printf '\010\000\000\000' | dd of="$ring" bs=1 seek=$((0x2200104)) conv=notrunc 2>/dev/null
printf '\002\000\000\000' | dd of="$ring" bs=1 seek=$((0x2200100)) conv=notrunc 2>/dev/null
sleep 1
echo "reply sequence-status $(dd if="$ring" bs=1 skip=$((0x2200080)) count=8 2>/dev/null | od -An -tu4)"
echo "cleared bytes $(dd if="$ring" bs=1 skip=$((0x2200200)) count=8 2>/dev/null | od -An -tu1)"
