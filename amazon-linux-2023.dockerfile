FROM docker.io/library/amazonlinux:2023 AS machnet_build_base

# Fixes QEMU-based builds so they don't emulate an x86-64-v1 CPU
ENV QEMU_CPU max

ARG timezone

# Set timezone and configure apt
RUN ln -snf /usr/share/zoneinfo/${timezone} /etc/localtime && \
    echo ${timezone} > /etc/timezone

# Update and install dependencies
RUN dnf update -y && \
    dnf install -y \
        git make automake gcc gcc-c++ kernel-devel ninja-build  \
        cmake meson pkg-config libudev-devel \
        libnl3-devel python3-devel \
        python3-docutils numactl-devel numactl \
        ca-certificates autoconf libasan libasan-static \
        pciutils libunwind-devel libuuid-devel xz-devel \
        python3-pip glibc-devel tar which iproute sudo

RUN python3 -m pip install pyelftools

ENV BUILDTYPE NATIVEONLY

ENV LIBHUGETLBFS_DIR /root/libhugetlbfs-2.24
ADD https://github.com/libhugetlbfs/libhugetlbfs/releases/download/2.24/libhugetlbfs-2.24.tar.gz /root
RUN cd /root && tar xf *.tar.gz && cd ${LIBHUGETLBFS_DIR} && \
    cd ${LIBHUGETLBFS_DIR} && \
    ./autogen.sh && ./configure && make obj/hugectl obj/hugeedit obj/hugeadm obj/pagesize && make install && \
    cd / && rm -rf ${LIBHUGETLBFS_DIR} /root/libhugetlbfs*.tar.gz

# libhugetlbfs is both picky and not particularly performance sensitive.
ARG CFLAGS
ARG CXXFLAGS

ENV NLOHMANN_JSON_DIR /root/nlohmann_json
ADD https://github.com/nlohmann/json.git#v3.10.5 ${NLOHMANN_JSON_DIR}
RUN cd ${NLOHMANN_JSON_DIR} && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && ninja install && cd / && rm -rf ${NLOHMANN_JSON_DIR}

# Remove conflicting packages
# RUN apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1

# Cleanup after package install
# RUN rm -rf /var/lib/apt/lists/*

WORKDIR /root

# Set env variable for rdma-core
ENV RDMA_CORE /root/rdma-core

# Build rdma-core
RUN git clone -b 'stable-v52' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE} && \
    cd ${RDMA_CORE} && \
    mkdir build && \
    cd build && \
    cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../ && \
    ninja install && \
    cd / && rm -rf ${RDMA_CORE}

# Set env variable for DPDK
ENV RTE_SDK /root/dpdk

# Parts of DPDK that aren't needed for Machnet are disabled to help with the container size and so that LTO hopefully has an easier time finding optimizations.

ENV DPDK_DISABLED_APPS dumpcap,graph,pdump,proc-info,test-acl,test-bbdev,test-cmdline,test-compress-perf,test-crypto-perf,test-dma-perf,test-eventdev,test-fib,test-flow-perf,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf
ENV DPDK_DISABLED_DRIVER_GROUPS raw/*,crypto/*,baseband/*,dma/*,event/*,regex/*,ml/*,gpu/*,vdpa/*,compress/*
ENV DPDK_DISABLED_COMMON_DRIVERS common/qat,common/octeontx,common/octeontx2,common/cnxk,common/dpaax
# probably the only safe bus driver to disable
ENV DPDK_DISABLED_BUS_DRIVERS bus/ifpga
# PMDs which don't meet the minimum requirements for Machnet
ENV DPDK_DISABLED_NIC_DRIVERS net/softnic,net/tap,net/af_packet,net/af_xdp,net/avp,net/bnx2x,net/memif,net/nfb,net/octeon_ep,net/pcap,net/ring,net/tap

# Additional drivers to disable. Intended to allow disabling drivers not needed in your environment to save on image size. This needs to end with a comma.
ARG DPDK_ADDITIONAL_DISABLED_DRIVERS

ENV DPDK_DISABLED_DRIVERS ${DPDK_ADDITIONAL_DISABLED_DRIVERS}${DPDK_DISABLED_DRIVER_GROUPS},${DPDK_DISABLED_COMMON_DRIVERS},${DPDK_DISABLED_BUS_DRIVERS},${DPDK_DISABLED_NIC_DRIVERS}

# Enabling a driver wins over disabling a driver, so if you the user disagree with any of our decisions add a comma delimited list of drivers to re-enable.
# For example, Marvell OcteonTX2 would be enabled by passing '--build-arg="DPDK_ENABLED_DRIVERS=common/octeontx2"' to the build command
ARG DPDK_ENABLED_DRIVERS

# Set the DPDK platform for ARM SOCs
ARG DPDK_PLATFORM=generic

# Additional Meson defines
ARG DPDK_EXTRA_MESON_DEFINES

# Preset to build DPDK with. Defaults to release mode with debug info.
ARG DPDK_MESON_BUILD_PRESET=debugoptimized

# Build DPDK
ADD https://github.com/DPDK/dpdk.git#v23.11 ${RTE_SDK}
# RUN git clone --depth 1 --branch 'v23.11' https://github.com/DPDK/dpdk.git ${RTE_SDK}
RUN cd ${RTE_SDK} && \
meson setup build --buildtype=${DPDK_MESON_BUILD_PRESET} -Dexamples='' -Dplatform=${DPDK_PLATFORM} -Denable_kmods=false -Dtests=false -Ddisable_apps=${DPDK_DISABLED_APPS} -Ddisable_drivers=${DPDK_DISABLED_DRIVERS} -Denable_drivers='${DPDK_ENABLED_DRIVERS}' ${DPDK_EXTRA_MESON_DEFINES} && \
    ninja -C build install && \
    cd / && \
    rm -rf ${RTE_SDK}

RUN echo /usr/local/lib64 > /etc/ld.so.conf.d/usr_local.conf && ldconfig

# Stage 2: Build Machnet
FROM machnet_build_base AS machnet

WORKDIR /root/machnet

# Copy Machnet files
COPY . .

# Submodule update
RUN git submodule update --init --recursive

# Do a Release build
RUN ldconfig && \
    mkdir release_build && \
    cd release_build && \
    cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && \
    ninja install machnet msg_gen pktgen shmem_test machnet_test

# Do a Debug build
RUN ldconfig && \
    mkdir debug_build && \
    cd debug_build && \
    cmake -DCMAKE_BUILD_TYPE=Debug -GNinja ../ && \
    ninja install machnet msg_gen pktgen shmem_test machnet_test

#
# Cleanup phase
#

RUN find /usr/local/lib64 -name "*/librte_*.a" | xargs rm -f


# ENTRYPOINT ["/root/machnet/release_build/src/apps/machnet/machnet"]

FROM scratch AS machnet_compressed_worker
COPY --from=machnet / /

CMD /root/machnet/release_build/src/apps/machnet/machnet --config_json /var/run/machnet/local_config.json
