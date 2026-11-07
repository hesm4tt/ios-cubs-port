#!/system/bin/sh
set -u
ring=$(mktemp /data/local/tmp/cubs-scan-test.XXXXXX) || exit 1
server=
client=
cleanup() {
  [ -z "$client" ] || kill "$client" 2>/dev/null || :
  [ -z "$server" ] || kill "$server" 2>/dev/null || :
  rm -f "$ring"
}
trap cleanup EXIT
header() { dd if="$ring" bs=1 skip=$((0x2100000)) count=12 2>/dev/null | od -An -tu4; }
CUBS_WIFI_RING="$ring" /data/local/tmp/wifiscansrv 10000 &
server=$!
sleep 1
kill -0 "$server" || exit 1
echo "initial $(header)"
/data/local/tmp/wifiscansrv_test 10001 >/dev/null 2>&1 &
client=$!
sleep 1
echo "unauthorized $(header)"
kill "$client" 2>/dev/null || :
wait "$client" 2>/dev/null || :
client=
/data/local/tmp/wifiscansrv_test 10000 >/dev/null 2>&1 &
client=$!
sleep 1
echo "authorized $(header)"
wait "$client" || exit 1
client=
sleep 1
echo "disconnected $(header)"
/data/local/tmp/wifiscansrv_test 10000 malformed >/dev/null 2>&1 &
client=$!
sleep 1
echo "malformed $(header)"
wait "$client" 2>/dev/null || :
client=
