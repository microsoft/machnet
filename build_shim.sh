#!/bin/bash

print_usage() {
  echo "Usage: ./build_shim.sh"
  echo ""
  echo "This script builds the Machnet shim library (libmachnet_shim.so) and examples"
}

function blue() {
  echo -e "\033[0;36m$1\033[0m"
}

if [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
  print_usage
  exit 0
fi

BASE_DIR="$(dirname "$(readlink -f "$0")")"
SRC_DIR="${BASE_DIR}/src/ext"

blue "Building Machnet shim library..."

cd "${SRC_DIR}" || exit 1
make clean
make

if [[ -f "${SRC_DIR}/libmachnet_shim.so" ]]; then
  cp "${SRC_DIR}/libmachnet_shim.so" "${BASE_DIR}/"
  blue "Machnet shim library built successfully and copied to ${BASE_DIR}/libmachnet_shim.so"
else
  echo "Error: Building Machnet shim library failed. Please check the build process."
  exit 1
fi

blue "Building Machnet examples..."
EXAMPLES_DIR="${BASE_DIR}/examples"
cd ${EXAMPLES_DIR}
make clean
make
blue "Machnet examples built successfully, see ${EXAMPLES_DIR} for binaries"
