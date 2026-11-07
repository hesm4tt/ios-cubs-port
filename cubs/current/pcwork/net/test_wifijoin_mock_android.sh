#!/system/bin/sh
set -eu
ring=$(mktemp /data/local/tmp/cubs-join-mock.XXXXXX)
marker="$ring.marker"
server=
cleanup() { [ -z "$server" ] || kill "$server" 2>/dev/null || :; rm -f "$ring" "$marker"; }
trap cleanup EXIT
CUBS_JOIN_RING="$ring" CUBS_JOIN_TEST_HELPER=/data/local/tmp/wifijoin_mock CUBS_JOIN_MOCK_MARKER="$marker" /data/local/tmp/wifijoinsrv_test &
server=$!
sleep 1
kill -0 "$server"
readword() { dd if="$ring" bs=1 skip="$1" count=4 2>/dev/null | od -An -tu4 | tr -d ' \n'; }
writeword() { printf "$1" | dd of="$ring" bs=1 seek="$2" conv=notrunc 2>/dev/null; }
frame() {
  printf 'JON1\004\000\000\000TEST' | dd of="$ring" bs=1 seek=$((0x2200200)) conv=notrunc 2>/dev/null
  writeword '\014\000\000\000' $((0x2200104))
}
waitword() {
  n=0
  while [ "$(readword "$1")" != "$2" ]; do
    n=$((n+1)); [ "$n" -lt 50 ] || return 1
    sleep 0.1
  done
}
frame
writeword '\003\000\000\000' $((0x2200180))
writeword '\002\000\000\000' $((0x2200100))
waitword $((0x2200080)) 2
[ "$(readword $((0x2200084)))" = 2 ] && [ ! -e "$marker" ]
echo cancel-before-claim-ok
frame
writeword '\000\000\000\000' $((0x2200180))
writeword '\004\000\000\000' $((0x2200100))
waitword $((0x22000c0)) 4
writeword '\004\000\000\000' $((0x2200180))
waitword $((0x2200080)) 4
[ "$(readword $((0x2200084)))" = 1 ] && [ -f "$marker" ]
[ "$(dd if="$ring" bs=1 skip=$((0x2200200)) count=12 2>/dev/null | od -An -tu1 | tr -d ' 0\n')" = '' ]
echo claim-and-commit-ok
