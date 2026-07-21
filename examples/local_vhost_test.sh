#!/bin/bash
#
# Local Machnet test over a vhost port -- no physical NIC, VM, or cloud
# instance required. This is the quickest way to develop and test Machnet
# applications on a single Linux machine.
#
# How it works
# ------------
# A single Machnet daemon is started with two DPDK *virtual* devices instead
# of a physical NIC (see local_vhost_config.json):
#
#   +-------------------- machnet daemon ---------------------+
#   |  engine 0 (10.66.0.1)          engine 1 (10.66.0.2)     |
#   |      |                              |                   |
#   |  net_vhost0  <-- unix socket -->  virtio_user1          |
#   +---------------------------------------------------------+
#         ^                                    ^
#         |                                    |
#   hello_world server                  hello_world client
#   (--local 10.66.0.1)                 (--local 10.66.0.2)
#
# `net_vhost0' is a vhost-user server backed by a unix socket;
# `virtio_user1' connects to that socket. Every packet transmitted by one
# port is received by the other, so the two Machnet engines behave like two
# machines on the same L2 segment. The hello_world client then sends a
# message from 10.66.0.2 to the server listening on 10.66.0.1, exercising
# the full Machnet datapath (ARP, handshake, data transfer).
#
# Notes
# -----
# * The net_vhost PMD derives its MAC address from its DPDK port id
#   (56:48:4f:53:54:<port id>). If machnet logs "port not found"-style
#   errors, compare the MAC keys in local_vhost_config.json with the
#   [PMDPORT] lines in the machnet log printed by this script.
# * Checksum offloads are unavailable on virtual devices, so packets carry
#   unfilled checksums. That is fine here: nothing on the vhost loop
#   verifies them. Do not use vhost ports to talk to a real network.
#
# Prerequisites: Linux, a Machnet build (see main README), hugepages.
#
# Usage: sudo ./local_vhost_test.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

MACHNET_BIN="${BUILD_DIR}/src/apps/machnet/machnet"
CONFIG_JSON="${SCRIPT_DIR}/local_vhost_config.json"
HELLO_WORLD="${SCRIPT_DIR}/hello_world"
VHOST_SOCK="/var/run/machnet/local_vhost.sock"
CTRL_SOCK="/var/run/machnet/machnet_ctrl.sock"

SERVER_IP="10.66.0.1"
CLIENT_IP="10.66.0.2"

LOG_DIR="$(mktemp -d /tmp/machnet_local_vhost.XXXXXX)"
MACHNET_LOG="${LOG_DIR}/machnet.log"
SERVER_LOG="${LOG_DIR}/hello_server.log"
CLIENT_LOG="${LOG_DIR}/hello_client.log"

die() { echo "ERROR: $*" >&2; exit 1; }

cleanup() {
    # Kill the hello_world server and the machnet daemon, if still running.
    [[ -n "${SERVER_PID:-}" ]] && kill "${SERVER_PID}" 2>/dev/null || true
    [[ -n "${MACHNET_PID:-}" ]] && kill "${MACHNET_PID}" 2>/dev/null || true
    # Give the daemon a moment to shut down and release hugepages.
    sleep 1
    rm -f "${VHOST_SOCK}"
    echo "Logs kept in ${LOG_DIR}"
}
trap cleanup EXIT

# ---------------------------------------------------------------- preflight
[[ "$(uname -s)" == "Linux" ]] || die "This test requires Linux (DPDK)."
[[ "$(id -u)" -eq 0 ]] || die "Please run as root: sudo $0"
[[ -x "${MACHNET_BIN}" ]] || \
    die "machnet binary not found at ${MACHNET_BIN}. Build the project first (see main README), or set BUILD_DIR."

if [[ ! -x "${HELLO_WORLD}" ]]; then
    echo "hello_world not built; building it now..."
    [[ -f "${REPO_ROOT}/libmachnet_shim.so" ]] || \
        die "libmachnet_shim.so not found in ${REPO_ROOT}. Run ${REPO_ROOT}/build_shim.sh first."
    make -C "${SCRIPT_DIR}" hello_world
fi

# Hugepages: DPDK needs some. Allocate 512 x 2MB pages if none are free.
free_hugepages=$(awk '/HugePages_Free/ {print $2}' /proc/meminfo)
if [[ "${free_hugepages:-0}" -eq 0 ]]; then
    echo "No free hugepages; allocating 512 x 2MB hugepages..."
    echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    free_hugepages=$(awk '/HugePages_Free/ {print $2}' /proc/meminfo)
    [[ "${free_hugepages}" -gt 0 ]] || die "Failed to allocate hugepages."
fi

if pgrep -x machnet > /dev/null; then
    die "Another machnet daemon is already running; stop it first."
fi

mkdir -p /var/run/machnet
rm -f "${VHOST_SOCK}" "${CTRL_SOCK}"

# ---------------------------------------------------- start the machnet daemon
echo "Starting machnet daemon (log: ${MACHNET_LOG})..."
GLOG_logtostderr=1 "${MACHNET_BIN}" --config_json "${CONFIG_JSON}" \
    &> "${MACHNET_LOG}" &
MACHNET_PID=$!

# Wait for the controller socket and for both ports to come up.
for _ in $(seq 1 60); do
    kill -0 "${MACHNET_PID}" 2>/dev/null || \
        { tail -n 30 "${MACHNET_LOG}"; die "machnet daemon exited early. Full log: ${MACHNET_LOG}"; }
    if [[ -S "${CTRL_SOCK}" ]] && \
       [[ "$(grep -c "Link is UP" "${MACHNET_LOG}" || true)" -ge 2 ]]; then
        break
    fi
    sleep 1
done
[[ -S "${CTRL_SOCK}" ]] || \
    { tail -n 30 "${MACHNET_LOG}"; die "machnet daemon did not come up. Full log: ${MACHNET_LOG}"; }
echo "machnet daemon is up (pid ${MACHNET_PID})."

# --------------------------------------------------------- run the experiment
echo "Starting hello_world server on ${SERVER_IP}..."
"${HELLO_WORLD}" --local "${SERVER_IP}" &> "${SERVER_LOG}" &
SERVER_PID=$!
sleep 2
kill -0 "${SERVER_PID}" 2>/dev/null || \
    { cat "${SERVER_LOG}"; die "hello_world server exited early."; }

echo "Sending a message from ${CLIENT_IP} to ${SERVER_IP}..."
"${HELLO_WORLD}" --local "${CLIENT_IP}" --remote "${SERVER_IP}" \
    &> "${CLIENT_LOG}" || { cat "${CLIENT_LOG}"; die "hello_world client failed."; }

# The server prints "Received message: Hello World!" when the message lands.
for _ in $(seq 1 10); do
    if grep -q "Received message" "${SERVER_LOG}"; then
        echo
        echo "PASS: message delivered over the local vhost loop:"
        grep "Received message" "${SERVER_LOG}"
        exit 0
    fi
    sleep 1
done

echo "---- machnet log (tail) ----";     tail -n 30 "${MACHNET_LOG}"
echo "---- server log ----";             cat "${SERVER_LOG}"
echo "---- client log ----";             cat "${CLIENT_LOG}"
die "Message was not received within 10 seconds."
