#!/bin/bash
# Start the NSaaS service on this machineA
# Usage: nsaas.sh --mac <local MAC> --ip <local IP>
#  - mac: MAC address of the local DPDK interface
#  - ip: IP address of the local DPDK interface
#  - bare_metal: if set, will use local binary instead of Docker image

LOCAL_MAC=""
LOCAL_IP=""
BARE_METAL=0
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
        *)
            echo "Unknown option $key"
            exit 1
            ;;
    esac
done

# Pre-flight checks
if [ -z "$LOCAL_MAC" ] || [ -z "$LOCAL_IP" ]; then
    echo "Please provide both local MAC and IP address"
    exit 1
fi

# Allocate memory for the first NUMA node
if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
then
    echo "Insufficient or no hugepages available"
    read -p "Do you want to allocate 1024 2MB hugepages? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]
    then
        echo "OK, continuing without allocating hugepages"
    else
        echo "Allocating 1024 hugepages"
        sudo bash -c "echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
        if ! cat /sys/devices/system/node/*/meminfo | grep HugePages_Total | grep -q 1024
        then
            echo "Failed to allocate hugepages"
            exit 1
        else
            echo "Successfully allocated 1024 hugepages on NUMA node0"
        fi
    fi
fi

# Allocate memory for the rest of the NUMA nodes if existed
for n in /sys/devices/system/node/node[1-9]; do
    if [ -d "$n" ]; then
        sudo bash -c "echo 1024 > $n/hugepages/hugepages-2048kB/nr_hugepages"
        if ! cat $n/meminfo | grep HugePages_Total | grep -q 1024 
        then
            echo "Failed to allocate hugepages on NUMA `echo $n | cut -d / -f 6`"
            exit 1
        else
            echo "Successfully allocated 1024 hugepages on NUMA `echo $n | cut -d / -f 6`"
        fi
    fi
done

if ! command -v docker &> /dev/null
then
    echo "Please install docker"
    exit
fi

if ! groups | grep -q docker; then
    echo "Please add the current user to the docker group"
    exit
fi

echo "Checking if the NSaaS Docker image is available"
if ! docker pull ghcr.io/msr-nsaas/nsaas:latest
then
    echo "Please make sure you have access to the NSaaS Docker image at ghcr.io/msr-nsaas/nsaas"
    echo "See NSaaS README for instructions on how to get access"
fi

echo "Starting NSaaS with local MAC $LOCAL_MAC and IP $LOCAL_IP"

if [ ! -d "/var/run/nsaas" ]; then
    echo "Creating /var/run/nsaas"
    sudo mkdir -p /var/run/nsaas
fi

sudo bash -c "echo '{\"nsaas_config\": {\"$LOCAL_MAC\": {\"ip\": \"$LOCAL_IP\"}}}' > /var/run/nsaas/local_config.json"
echo "Created config for local NSaaS, in /var/run/nsaas/local_config.json. Contents:"
sudo cat /var/run/nsaas/local_config.json

if [ $BARE_METAL -eq 1 ]; then
    echo "Starting NSaaS in bare metal mode"
    THIS_SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
    nsaas_bin="$THIS_SCRIPT_DIR/build/src/apps/nsaas/nsaas"

    if [ ! -f ${nsaas_bin} ]; then
        echo "${nsaas_bin} not found, please build NSaaS first"
        exit 1
    fi

    sudo ${nsaas_bin} --config_json /var/run/nsaas/local_config.json --logtostderr=1
else
    sudo docker run --privileged --net=host \
        -v /dev/hugepages:/dev/hugepages \
        -v /var/run/nsaas:/var/run/nsaas \
        ghcr.io/msr-nsaas/nsaas:latest \
        /root/nsaas/build/src/apps/nsaas/nsaas \
        --config_json /var/run/nsaas/local_config.json \
        --logtostderr=1
fi