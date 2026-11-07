#!/system/bin/sh
# Do not print network information or change Wi-Fi state.
if cmd wifi status >/dev/null 2>&1; then
  echo root-wifi-service-available
else
  echo root-wifi-service-unavailable
fi
