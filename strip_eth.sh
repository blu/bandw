#!/bin/bash

if (( $# != 1 )); then
	echo "usage: $0 iface_dev"
	exit -1
fi

ip link show | grep -E "^[[:digit:]]+: "$1 > /dev/null

if (( $? == 1 )); then
	echo "no such interface"
	exit -1
fi

# take down the interface
sudo ip link set $1 down

# ipv6 can autostart - disable that
sudo sysctl -q -w net.ipv6.conf.$1.disable_ipv6=1

# bring up the interface in bare link mode
sudo ip link set $1 up
