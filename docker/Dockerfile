FROM ubuntu:20.04
ARG timezone

RUN ln -snf /usr/share/zoneinfo/${timezone} /etc/localtime && echo ${timezone} > /etc/timezone

# Try to reduce the image size by not installing recommends and suggests packages
RUN echo 'APT::Install-Suggests "0";' >> /etc/apt/apt.conf.d/00-docker
RUN echo 'APT::Install-Recommends "0";' >> /etc/apt/apt.conf.d/00-docker
RUN apt-get update && apt-get install --no-install-recommends

RUN apt-get install -y git build-essential cmake meson pkg-config libudev-dev \
    libnl-3-dev libnl-route-3-dev python3-dev \
    python3-docutils python3-pyelftools libnuma-dev
RUN apt-get install -y ca-certificates

# Build uWebSockets from source
# WORKDIR /root
# RUN git clone --depth=1 --recurse-submodules -b v20.29.0 https://github.com/uNetworking/uWebSockets.git
# WORKDIR uWebSockets
# RUN make
# RUN rm -rf fuzzing .git

# Build cityhash from source
WORKDIR /root
RUN git clone --depth=1 --recurse-submodules https://github.com/google/cityhash.git
WORKDIR cityhash
RUN ./configure --enable-sse4.2
RUN make all check CXXFLAGS="-O3 -msse4.2"

# Build rdma-core
ENV RDMA_CORE /root/rdma-core
RUN date
RUN git clone -b 'stable-v40' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE}
WORKDIR ${RDMA_CORE}
RUN mkdir build
WORKDIR build/
RUN cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../ && ninja install

# Build DPDK
ENV RTE_SDK /root/dpdk
RUN git clone --depth 1 --branch 'v21.11' https://github.com/DPDK/dpdk.git ${RTE_SDK}
WORKDIR ${RTE_SDK}
RUN meson build -Dexamples='' -Dplatform=generic -Denable_kmods=false -Dtests=false -Ddisable_drivers='raw/*,crypto/*,baseband/*,dma/*'
WORKDIR build/
RUN DESTDIR=${RTE_SDK}/build/install ninja install
RUN rm -rf ${RTE_SDK}/app ${RTE_SDK}/drivers ${RTE_SDK}/.git ${RTE_SDK}/build/app

# Build NSaaS
RUN apt-get install -y libgflags-dev libgflags2.2 libhugetlbfs-dev pciutils libunwind-dev uuid-dev nlohmann-json3-dev
RUN apt-get install -y gcc-10 g++-10
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100
RUN update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100
RUN update-alternatives --install /usr/bin/gcov gcov /usr/bin/gcov-10 100

# Image/nsaas contains all the NSaaS repo files needed to build the container
ADD Image /root
WORKDIR /root/nsaas
RUN mkdir build
WORKDIR build
RUN cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && ninja

# # Run test applicaton.
# RUN env GLOG_logtostderr=1 ./src/apps/dummy/dummy
