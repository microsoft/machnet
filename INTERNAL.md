This document is for developers of the Machnet service. It is not intended for
users of the Machnet service. For users, see the [README](README.md).

## Compiling Machnet


```bash
# On Ubuntu:
sudo apt -y install cmake libgflags-dev pkg-config nlohmann-json3-dev ninja-build gcc-10 g++-10 doxygen graphviz python3-pip meson libhugetlbfs-dev
pip3 install pyelftools
```


  For Ubuntu 20.04 we need `gcc-10`, `g++-10`, `cpp-10`.
  ```bash
  # Set gcc-10 and g++-10 as the default compiler.
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10 --slave /usr/bin/gcov gcov /usr/bin/gcov-10
  ```

### Build DPDK

We use DPDK v21.11 LTS, like so:

```bash
export RTE_SDK=/path/to/dpdk-21.11/
cd ${RTE_SDK} && meson build && cd build && ninja && DESTDIR=${PWD}/install ninja install
```

### Build Machnet

Then, from `${REPOROOT}`:
```bash
git submodule update --init --recursive

# Debug build:
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug -GNinja ../ && ninja

# Release build:
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && ninja
```

The `machnet` binary will be available in `${REPOROOT}/build/src/apps/`.  You may
see more details about the `Machnet` program in this
[README](src/apps/machnet/README.md).

## Tests

To run all tests, from ${REPOROOT}:
```bash
# If the Ansible automation was not used, enable hugepages first
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo ctest # sudo is required for DPDK-related tests.
```

## Docker
```bash
cd docker
./dockerbuild.sh
```


## Development

The codebase follows [Google's C++
standard](https://google.github.io/styleguide/cppguide.html). Some tools are
required for linting, checking, code formatting:

```bash
sudo apt install clang-format cppcheck # Or equivalent for your OS
pip install pre-commit
cd ${REPOROOT}
pre-commit install
```
