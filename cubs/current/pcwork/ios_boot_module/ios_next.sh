#!/system/bin/sh
# "Reboot into iOS": leave the flag for the boot hook and reboot (run as root: `adb shell su -c "sh /data/local/ios/ios_next.sh"` or through ksud)
touch /data/local/ios/boot_ios
sync
reboot
