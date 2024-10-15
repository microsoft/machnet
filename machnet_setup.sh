#!/usr/bin/env bash

sudo apt-get update

sudo apt-get install --no-install-recommends -y \
        git \
        build-essential cmake meson pkg-config libudev-dev \
        libnl-3-dev libnl-route-3-dev python3-dev \
        python3-docutils python3-pyelftools libnuma-dev \
        ca-certificates autoconf \
        libhugetlbfs-dev pciutils libunwind-dev uuid-dev nlohmann-json3-dev
sudo apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1

sudo rm -rf /var/lib/apt/lists/*

export RDMA_CORE=~/rdma-core


git clone -b 'stable-v52' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE}
cd ${RDMA_CORE}
mkdir build
cd build
cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../
sudo ninja install

echo /usr/local/lib64 | sudo tee /etc/ld.so.conf.d/usr_local.conf > /dev/null && sudo ldconfig

cd $HOME

export RTE_SDK=~/dpdk
# Set environment variables for disabled DPDK apps, drivers, and configurations
export DPDK_DISABLED_APPS="dumpcap,graph,pdump,proc-info,test-acl,test-bbdev,test-cmdline,test-compress-perf,test-crypto-perf,test-dma-perf,test-eventdev,test-fib,test-flow-perf,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf"
export DPDK_DISABLED_DRIVER_GROUPS="raw/*,crypto/*,baseband/*,dma/*,event/*,regex/*,ml/*,gpu/*,vdpa/*,compress/*"
export DPDK_DISABLED_COMMON_DRIVERS="common/qat,common/octeontx,common/octeontx2,common/cnxk,common/dpaax"
export DPDK_DISABLED_BUS_DRIVERS="bus/ifpga"
export DPDK_DISABLED_NIC_DRIVERS="net/softnic,net/tap,net/af_packet,net/af_xdp,net/avp,net/bnx2x,net/memif,net/nfb,net/octeon_ep,net/pcap,net/ring,net/tap"
# Accept additional disabled drivers via argument (empty if not provided)
DPDK_ADDITIONAL_DISABLED_DRIVERS=${1:-""}
# Combine all disabled drivers into one variable
export DPDK_DISABLED_DRIVERS="${DPDK_ADDITIONAL_DISABLED_DRIVERS}${DPDK_DISABLED_DRIVER_GROUPS},${DPDK_DISABLED_COMMON_DRIVERS},${DPDK_DISABLED_BUS_DRIVERS},${DPDK_DISABLED_NIC_DRIVERS}"
# Accept enabled drivers as an argument (optional)
DPDK_ENABLED_DRIVERS=${2:-""}
# Set the DPDK platform for ARM SOCs (default to 'generic')
DPDK_PLATFORM=${3:-"generic"}
# Additional Meson defines (optional)
DPDK_EXTRA_MESON_DEFINES=${4:-""}
# Set the build preset for DPDK (defaults to 'debugoptimized')
DPDK_MESON_BUILD_PRESET=${5:-"debugoptimized"}
# Display the configuration to verify the values
echo "Disabled Apps: $DPDK_DISABLED_APPS"
echo "Disabled Drivers: $DPDK_DISABLED_DRIVERS"
echo "Enabled Drivers: $DPDK_ENABLED_DRIVERS"
echo "DPDK Platform: $DPDK_PLATFORM"
echo "Extra Meson Defines: $DPDK_EXTRA_MESON_DEFINES"
echo "Build Preset: $DPDK_MESON_BUILD_PRESET"
git clone --depth 1 --branch 'v23.11' https://github.com/DPDK/dpdk.git ${RTE_SDK}
cd ${RTE_SDK}
meson setup build --buildtype=${DPDK_MESON_BUILD_PRESET} -Dexamples='' -Dplatform=${DPDK_PLATFORM} -Denable_kmods=false -Dtests=false -Ddisable_apps=${DPDK_DISABLED_APPS} -Ddisable_drivers=${DPDK_DISABLED_DRIVERS} -Denable_drivers='${DPDK_ENABLED_DRIVERS}' ${DPDK_EXTRA_MESON_DEFINES}
sudo ninja -C build install
sudo ldconfig
