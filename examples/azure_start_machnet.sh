#!/bin/bash
# Hacky script to start-up Machnet on an Azure VM on interface #1

machnet_ip_addr=$(
    curl -s -H Metadata:true --noproxy "*" \
    "http://169.254.169.254/metadata/instance?api-version=2021-02-01" | \
    jq '.network.interface[1].ipv4.ipAddress[0].privateIpAddress' | \
    tr -d '"')
echo "Machnet IP address: $machnet_ip_addr"

machnet_mac_addr=$(
    curl -s -H Metadata:true --noproxy "*" \
    "http://169.254.169.254/metadata/instance?api-version=2021-02-01" | \
    jq '.network.interface[1].macAddress' | \
    tr -d '"' | \
    sed 's/\(..\)/\1:/g;s/:$//') # Converts AABBCCDDEEFF to AA:BB:CC:DD:EE:FF
echo "Machnet MAC address: $machnet_mac_addr"

sudo modprobe uio_hv_generic

if [ -d /sys/class/net/eth1 ]; then
    DEV_UUID=$(basename $(readlink /sys/class/net/eth1/device))
    echo "Unbinding $DEV_UUID from hv_netvsc"
    sudo driverctl -b vmbus set-override $DEV_UUID uio_hv_generic
fi

cd ..; ./machnet.sh --bare_metal --mac $machnet_mac_addr --ip $machnet_ip_addr
