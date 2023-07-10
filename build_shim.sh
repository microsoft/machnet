#!/bin/bash

print_usage() {
  echo "Usage: ./build_shim.sh"
  echo ""
  echo "This script builds the NSaaS shim library (libnsaas_shim.so) and examples"
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

blue "Building NSaaS shim library..."

cd "${SRC_DIR}" || exit 1
make clean
make

if [[ -f "${SRC_DIR}/libnsaas_shim.so" ]]; then
  cp "${SRC_DIR}/libnsaas_shim.so" "${BASE_DIR}/"
  blue "NSaaS shim library built successfully and copied to ${BASE_DIR}/libnsaas_shim.so"
else
  echo "Error: Building NSaaS shim library failed. Please check the build process."
  exit 1
fi

blue "Building NSaaS examples..."
EXAMPLES_DIR="${BASE_DIR}/examples"
cd ${EXAMPLES_DIR} 
make clean
make
blue "NSaaS examples built successfully, see ${EXAMPLES_DIR} for binaries"