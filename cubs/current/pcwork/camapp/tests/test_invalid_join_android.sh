#!/system/bin/sh
# Malformed frame only; this cannot request a network connection.
printf 'BAD!\000\000\000\000' | CLASSPATH=/data/local/tmp/cubs-wifi-join-helper.dex /system/bin/app_process /system/bin com.cubs.cam.PrivilegedWifiJoin
