#!/bin/bash
# Start the Machnet service on this machineA
# Usage: machnet.sh --mac <local MAC> --ip <local IP>
#  - mac: MAC address of the local DPDK interface
#  - ip: IP address of the local DPDK interface
#  - bare_metal: if set, will use local binary instead of Docker image

MACHNET_CONF_DIR="/mnt/machnet"
MACHNET_RUN_DIR="/var/run/machnet"

LOCAL_MAC=""
LOCAL_IP=""

azure_get_ip_from_mac() {

    # Convert MAC address to Azure's format (remove ':' and convert to uppercase)
    MAC_ADDRESS=$(echo "$1" | tr -d ':' | tr 'a-f' 'A-F')

    # Get the interface data from Azure's Instance Metadata Service using curl
    VM_METADATA=$(curl -s --max-time 5 -H "Metadata:true" "http://169.254.169.254/metadata/instance/network/interface?api-version=2020-09-01")

    # Use jq to parse the IP from the data
    IP_ADDRESS=$(echo "$VM_METADATA" | jq -r ".[] | select(.macAddress == \"$MAC_ADDRESS\") | .ipv4.ipAddress[0].privateIpAddress")

    echo "$IP_ADDRESS"
}

baremetal_get_ip_from_mac() {
    TARGET_MAC="$1"
    FOUND_IP=""  # This will store the found IP address

    # Return immediately if no MAC address provided
    if [[ -z "$TARGET_MAC" ]]; then
        echo ""
        return
    fi

    # Convert MAC address to lowercase for matching, as sysfs represents MAC in lowercase
    TARGET_MAC=$(echo "$TARGET_MAC" | tr 'A-F' 'a-f')

    # Walk through all interfaces
    for iface in /sys/class/net/*; do
        # Get the MAC address for this interface
        MAC_ADDRESS=$(cat "$iface/address")

        # Check if this is the MAC address we're looking for
        if [[ "$MAC_ADDRESS" == "$TARGET_MAC" ]]; then
            # Extract the interface name from the path
            INTERFACE_NAME=$(basename $iface)

            # Use `ip addr show` to get the IP address for this interface
            IP_ADDRESS=$(ip addr show $INTERFACE_NAME | grep 'inet ' | awk '{print $2}' | cut -d'/' -f1)

            # If IP_ADDRESS is not empty, set FOUND_IP and break
            if [[ ! -z "$IP_ADDRESS" ]]; then
                FOUND_IP=$IP_ADDRESS
                break
            fi
        fi
    done

    echo "$FOUND_IP"
}

get_inteface_from_ip() {
    TARGET_IP="$1"

    # Return immediately if no MAC address provided
    if [[ -z "$TARGET_IP" ]]; then
        return ""
    fi

    INTERFACE_NAME=$(ip -brief -f inet -4 addr show | grep $TARGET_IP | awk '{print $1}')

    echo "$INTERFACE_NAME"
}

is_running_on_azure() {
    # Set the metadata URL and API version
    AZURE_METADATA_URL="http://169.254.169.254/metadata/instance?api-version=2020-09-01"

    # Use curl to send a request. We're using the `-s` flag for silent mode, `-f` to fail silently on server errors,
    # and the header "Metadata:true" which is required by Azure's service.
    # Additionally, `--max-time 5` ensures the request times out after 5 seconds.
    RESPONSE=$(curl -s -f --max-time 5 -H "Metadata:true" "$AZURE_METADATA_URL")

    # Check the HTTP status code. Azure's metadata service should return 200.
    if [ $? -eq 0 ]; then
        return 0  # Return true (running on Azure)
    else
        return 1  # Return false (not running on Azure)
    fi
}

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

IS_AZURE=$(is_running_on_azure)

# Pre-flight checks
if [ -z "$LOCAL_MAC" ]; then
    echo "Please provide MAC address."
    exit 1
fi

# Check if running on azure.
if $IS_AZURE ; then
    # Check if jq is not installed and install it.
    if ! command -v jq &> /dev/null
    then
        echo "jq not found, installing jq"
        sudo apt-get install -y jq
    fi
fi

if [ -z "$LOCAL_IP" ]; then
    # check if IS_AZURE is true
    if $IS_AZURE ; then
        echo "Running on Azure, getting IP address from MAC address"
        LOCAL_IP=$(azure_get_ip_from_mac "$LOCAL_MAC")
    else
        echo "Not running on Azure, getting IP address from MAC address"
        LOCAL_IP=$(baremetal_get_ip_from_mac "$LOCAL_MAC")
    fi
fi

if [ -z "$LOCAL_IP" ]; then
    echo "Failed to get IP address. Explicitly provide both MAC and IP address."
    exit 1
fi

#
# Hugepage allocation
#

#Allocate memory for the first NUMA node
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

# Allocate memory for the rest of the NUMA nodes, if any
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

# If we're running on Azure, we need to use uio_hv_generic and detach the interface.
if $IS_AZURE; then
    echo "Running on Azure, using uio_hv_generic"
    sudo modprobe uio_hv_generic
    bash -c "echo uio_hv_generic | sudo tee /sys/bus/vmbus/drivers/uio_hv_generic/new_id"
    bash -c "echo $LOCAL_MAC | sudo tee /sys/bus/vmbus/drivers/hv_netvsc/unbind"
    bash -c "echo $LOCAL_MAC | sudo tee /sys/bus/vmbus/drivers/uio_hv_generic/bind"
fi

echo "Starting Machnet with local MAC $LOCAL_MAC and IP $LOCAL_IP"

if [ ! -d ${MACHNET_CONF_DIR} ]; then
    echo "Creating ${MACHNET_CONF_DIR}"
    sudo mkdir -p ${MACHNET_CONF_DIR}
    sudo chgrp -R docker ${MACHNET_CONF_DIR}
    sudo chmod 755 ${MACHNET_CONF_DIR}
fi

if [ ! -d ${MACHNET_RUN_DIR} ]; then
    echo "Creating ${MACHNET_RUN_DIR}"
    sudo mkdir -p ${MACHNET_RUN_DIR}
    sudo chgrp -R docker ${MACHNET_RUN_DIR}
    sudo chmod 755 ${MACHNET_RUN_DIR}
fi

bash -c "echo '{\"machnet_config\": {\"$LOCAL_MAC\": {\"ip\": \"$LOCAL_IP\"}}}' | sudo tee ${MACHNET_CONF_DIR}/config.json"
echo "Created config for local Machnet, in ${MACHNET_CONF_DIR}/config.json. Contents:"
cat ${MACHNET_CONF_DIR}/config.json

if ! command -v docker &> /dev/null
then
    echo "Please install docker"
    exit
fi

if ! groups | grep -q docker; then
    echo "Please add the current user to the docker group"
    exit
fi

echo "Checking if the Machnet Docker image is available"
if ! docker pull ghcr.io/microsoft/machnet/machnet:latest
then
    echo "Please make sure you have access to the Machnet Docker image at ghcr.io/microsoft/machnet/"
    echo "See Machnet README for instructions on how to get access"
fi

docker run --privileged --net=host \
    -v /dev/hugepages:/dev/hugepages \
    -v ${MACHNET_CONF_DIR}:${MACHNET_CONF_DIR} \
    -v ${MACHNET_RUN_DIR}:${MACHNET_RUN_DIR} \
    ghcr.io/microsoft/machnet/machnet:latest \
    /root/machnet/build/src/apps/machnet/machnet \
    --config_json ${MACHNET_CONF_DIR}/config.json \
    --logtostderr=1
