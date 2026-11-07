chmod 755 /data/local/tmp/drmfb
stop surfaceflinger; stop vendor.hwcomposer-3; sleep 2
( /data/local/tmp/drmfb /data/local/tmp/fblist.bin hold > /data/local/tmp/drmfb.log 2>&1 & )
sleep 6
cat /data/local/tmp/drmfb.log
