#!/system/bin/sh
# Android's end of en0: tun device ios0 (10.77.0.1/24), forwarding, NAT out through whatever network Android is using, and the netsrv daemon.
# Run as root, from ios_start.sh.
D=/data/local/ios
exec > $D/netsrv.log 2>&1
cd $D
pkill netsrv 2>/dev/null
./netsrv &
sleep 1
ip addr add 10.77.0.1/24 dev ios0
ip link set ios0 up mtu 1500
echo 1 > /proc/sys/net/ipv4/ip_forward
# the answers come back in through the network in use, and Android has no route for 10.77.0.0/24 for them: send them to ios0
ip rule del to 10.77.0.0/24 2>/dev/null
ip rule add to 10.77.0.0/24 lookup main pref 8000
iptables -I FORWARD 1 -i ios0 -j ACCEPT
iptables -I FORWARD 1 -o ios0 -j ACCEPT
iptables -t nat -I POSTROUTING 1 -s 10.77.0.0/24 ! -o ios0 -j MASQUERADE
# Android routes by policy: traffic that comes in on ios0 has no mark, so it goes to the table of the network in use
(
	last=
	while true; do
		dev=
		for c in wlan0 v4-wwan1 v4-wwan2 v4-wwan3 wwan1 wwan2 wwan3; do
			if ip route show table all 2>/dev/null | grep -q -E "^default via [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+ dev $c "; then dev=$c; break; fi
		done
		if [ -n "$dev" ] && [ "$dev" != "$last" ]; then
			ip rule del iif ios0 2>/dev/null
			tbl=$((1000 + $(ip -o link show $dev | cut -d: -f1)))
			ip rule add iif ios0 lookup $tbl pref 9000
			echo "rule: iif ios0 -> table $dev ($tbl)"
			last=$dev
		fi
		sleep 15
	done
) &
wait
