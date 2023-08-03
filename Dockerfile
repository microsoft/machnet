FROM ubuntu:22.04

ARG timezone

# Set timezone and configure apt
RUN ln -snf /usr/share/zoneinfo/${timezone} /etc/localtime && \
    echo ${timezone} > /etc/timezone && \
    echo 'APT::Install-Suggests "0";' >> /etc/apt/apt.conf.d/00-docker && \
    echo 'APT::Install-Recommends "0";' >> /etc/apt/apt.conf.d/00-docker 

# Update and install dependencies
RUN apt-get update && \
    apt-get install --no-install-recommends -y \
        git \
        build-essential cmake meson pkg-config libudev-dev \
        libnl-3-dev libnl-route-3-dev python3-dev \
        python3-docutils python3-pyelftools libnuma-dev \
        ca-certificates autoconf \
        libgflags-dev libgflags2.2 libhugetlbfs-dev pciutils libunwind-dev uuid-dev nlohmann-json3-dev 

# Remove conflicting packages
RUN apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1

# Cleanup after package install
RUN rm -rf /var/lib/apt/lists/*

WORKDIR /root

# Build cityhash from source
RUN git clone --depth=1 --recurse-submodules https://github.com/google/cityhash.git && \
    cd cityhash && \
    ./configure --enable-sse4.2 && \
    make all check CXXFLAGS="-O3 -msse4.2"

# Set env variable for rdma-core
ENV RDMA_CORE /root/rdma-core

# Build rdma-core
RUN git clone -b 'stable-v40' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE} && \
    cd ${RDMA_CORE} && \
    mkdir build && \
    cd build && \
    cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../ && \
    ninja install

# Set env variable for DPDK
ENV RTE_SDK /root/dpdk

# Build DPDK
RUN git clone --depth 1 --branch 'v21.11' https://github.com/DPDK/dpdk.git ${RTE_SDK} && \
    cd ${RTE_SDK} && \
    meson build -Dexamples='' -Dplatform=generic -Denable_kmods=false -Dtests=false -Ddisable_drivers='raw/*,crypto/*,baseband/*,dma/*' && \
    cd build/ && \
    DESTDIR=${RTE_SDK}/build/install ninja install && \
    rm -rf ${RTE_SDK}/app ${RTE_SDK}/drivers ${RTE_SDK}/.git ${RTE_SDK}/build/app

WORKDIR /root/machnet

# Copy Machnet files
COPY . .

# Build Machnet
RUN mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && \
    ninja
