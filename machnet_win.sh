#!/bin/bash
# Start the Machnet service on this machineA
# Usage: machnet.sh --mac <local MAC> --ip <local IP>
#  - mac: MAC address of the local DPDK interface
#  - ip: IP address of the local DPDK interface
#  - debug: if set, run a debug build from the Machnet Docker container
#  - bare_metal: if set, will use local binary instead of Docker image

LOCAL_MAC="60-45-BD-83-26-DD"
LOCAL_IP="10.2.0.17"
BARE_METAL=1
DEBUG=0
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -m|--mac)
            LOCAL_MAC="$2"
            shift
            shift
            ;;
        -i|--ip)
            LOCAL_IP="$2"
            shift
            shift
            ;;
        -b|--bare_metal)
            BARE_METAL=1
            shift
            ;;
        -d|--debug)
            DEBUG=1
            shift
            ;;
        *)
            echo "Unknown option $key"
            exit 1
            ;;
    esac
done

# Pre-flight checks
if [ -z "$LOCAL_MAC" ] || [ -z "$LOCAL_IP" ]; then
    echo "Usage: machnet.sh --mac <local MAC> --ip <local IP>" 
    exit 1
fi

#
# Hugepage allocation
#

#Allocate memory for the first NUMA node
# if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
# then
#     echo "Insufficient or no hugepages available"
#     read -p "Do you want to allocate 1024 2MB hugepages? (y/n) " -n 1 -r
#     echo
#     if [[ ! $REPLY =~ ^[Yy]$ ]]
#     then
#         echo "OK, continuing without allocating hugepages"
#     else
#         echo "Allocating 1024 hugepages"
#         sudo bash -c "echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
#         if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
#         then
#             echo "Failed to allocate hugepages"
#             exit 1
#         else
#             echo "Successfully allocated 1024 hugepages on NUMA node0"
#         fi
#     fi
# fi

# Allocate memory for the rest of the NUMA nodes, if any
# for n in /sys/devices/system/node/node[1-9]; do
#     if [ -d "$n" ]; then
#         sudo bash -c "echo 1024 > $n/hugepages/hugepages-2048kB/nr_hugepages"
#         if ! cat $n/meminfo | grep HugePages_Total | grep -q 1024
#         then
#             echo "Failed to allocate hugepages on NUMA `echo $n | cut -d / -f 6`"
#             exit 1
#         else
#             echo "Successfully allocated 1024 hugepages on NUMA `echo $n | cut -d / -f 6`"
#         fi
#     fi
# done


echo "Starting Machnet with local MAC $LOCAL_MAC and IP $LOCAL_IP"

if [ ! -d "./var/run/machnet" ]; then
    echo "Creating /var/run/machnet"
    mkdir -p ./var/run/machnet
    chmod 755 ./var/run/machnet # Set permissions like Ubuntu's default, needed on (e.g.) CentOS
fi

# bash -c "echo '{\"machnet_config\": {\"$LOCAL_MAC\": {\"ip\": \"$LOCAL_IP\"}}}' > ./var/run/machnet/local_config.json"
# echo "Created config for local Machnet, in /var/run/machnet/local_config.json. Contents:"
# cat /var/run/machnet/local_config.json

# chmod 755 ./var/run/machnet/local_config.json

if [ $BARE_METAL -eq 1 ]; then
    echo "Starting Machnet in bare-metal mode"
    # THIS_SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
    machnet_bin="./build/bin/machnet.exe"

    if [ ! -f ${machnet_bin} ]; then
        echo "Machnet binary ${machnet_bin} not found, please build Machnet first"
        exit 1
    fi

    ${machnet_bin} --config_json src/apps/machnet/config.json 
fi
