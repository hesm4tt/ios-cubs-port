#!/system/bin/sh
set -e
echo malformed-frame
sh /data/local/tmp/test_wifijoinsrv_android.sh
echo synthetic-mock
sh /data/local/tmp/test_wifijoin_mock_android.sh
ring=$(mktemp /data/local/tmp/cubs-permission-test.XXXXXX)
trap 'rm -f "$ring"' EXIT
chmod 666 "$ring"
if CUBS_JOIN_RING="$ring" /data/local/tmp/wifijoinsrv >/dev/null 2>&1; then
  echo unsafe-ring-accepted
  exit 1
fi
echo unsafe-ring-rejected
