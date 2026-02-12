/**
 * @file tcp_msg_gen.cc
 * @brief Standalone Linux TCP message generator that speaks the same
 *        wire protocol as Machnet's msg_gen running with --protocol=tcp.
 *
 * Wire format (Machnet TCP framing):
 *   [4-byte big-endian message length][payload]
 *
 * The payload starts with an 8-byte app_hdr_t (window_slot), followed by
 * filler bytes up to --msg_size.
 *
 * Usage:
 *   Server (pairs with Machnet msg_gen client):
 *     ./tcp_msg_gen --local_ip=0.0.0.0 --local_port=888 --msg_size=64
 *
 *   Client (pairs with Machnet msg_gen server):
 *     ./tcp_msg_gen --local_ip=0.0.0.0 --remote_ip=10.0.0.2 \
 *                   --remote_port=888 --msg_size=64 --msg_window=8
 */

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <vector>

// ──────── Configuration (mirrors msg_gen flags) ────────

static const char *g_local_ip = "0.0.0.0";
static const char *g_remote_ip = nullptr;
static uint16_t g_local_port = 888;
static uint16_t g_remote_port = 888;
static uint32_t g_msg_size = 64;
static uint32_t g_msg_window = 8;

static volatile int g_keep_running = 1;
void sig_handler(int) { g_keep_running = 0; }

// ──────── app_hdr_t must match msg_gen exactly ────────

struct app_hdr_t {
  uint64_t window_slot;
};

// ──────── Helpers ────────

/// Read exactly `len` bytes (blocking).
static bool read_exact(int fd, void *buf, size_t len) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

/// Write exactly `len` bytes (blocking).
static bool write_exact(int fd, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

/// Receive one framed message: 4-byte BE length prefix + payload.
/// Returns payload size, or -1 on error/disconnect.
static ssize_t recv_framed(int fd, void *buf, size_t buf_cap) {
  uint32_t net_len;
  if (!read_exact(fd, &net_len, 4)) return -1;
  uint32_t msg_len = be32toh(net_len);
  if (msg_len > buf_cap) {
    fprintf(stderr, "Message too large: %u > %zu\n", msg_len, buf_cap);
    return -1;
  }
  if (!read_exact(fd, buf, msg_len)) return -1;
  return static_cast<ssize_t>(msg_len);
}

/// Send one framed message: 4-byte BE length prefix + payload.
static bool send_framed(int fd, const void *buf, size_t len) {
  uint32_t net_len = htobe32(static_cast<uint32_t>(len));
  if (!write_exact(fd, &net_len, 4)) return false;
  if (!write_exact(fd, buf, len)) return false;
  return true;
}

// ──────── Stats ────────

struct Stats {
  uint64_t tx_count = 0, tx_bytes = 0;
  uint64_t rx_count = 0, rx_bytes = 0;
};

static void report_stats(Stats &cur, Stats &prev,
                         std::chrono::steady_clock::time_point &last_time) {
  auto now = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double>(now - last_time).count();
  if (elapsed < 1.0) return;

  double tx_kmps = (cur.tx_count - prev.tx_count) / (1000 * elapsed);
  double rx_kmps = (cur.rx_count - prev.rx_count) / (1000 * elapsed);
  double tx_gbps =
      ((cur.tx_bytes - prev.tx_bytes) * 8) / (elapsed * 1E9);
  double rx_gbps =
      ((cur.rx_bytes - prev.rx_bytes) * 8) / (elapsed * 1E9);

  std::cout << "TX/RX (msg/sec, Gbps): (" << std::fixed
            << std::setprecision(1) << tx_kmps << "K/" << rx_kmps << "K"
            << std::fixed << std::setprecision(3) << ", " << tx_gbps << "/"
            << rx_gbps << ")" << std::endl;

  prev = cur;
  last_time = now;
}

// ──────── Server mode ────────
// Listens, accepts one connection, echoes back every message.

static int run_server() {
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) { perror("socket"); return 1; }

  int opt = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(listenfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_local_port);
  inet_pton(AF_INET, g_local_ip, &addr.sin_addr);

  if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind"); return 1;
  }
  if (listen(listenfd, 1) < 0) { perror("listen"); return 1; }

  std::cout << "[SERVER] Listening on " << g_local_ip << ":" << g_local_port
            << ", msg_size=" << g_msg_size << std::endl;

  struct sockaddr_in peer{};
  socklen_t peer_len = sizeof(peer);
  int connfd = accept(listenfd, (struct sockaddr *)&peer, &peer_len);
  if (connfd < 0) { perror("accept"); return 1; }

  opt = 1;
  setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  char peer_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
  std::cout << "[SERVER] Accepted connection from " << peer_ip << ":"
            << ntohs(peer.sin_port) << std::endl;

  std::vector<uint8_t> rx_buf(8 * 1024 * 1024);
  std::vector<uint8_t> tx_buf(g_msg_size);
  std::iota(tx_buf.begin(), tx_buf.end(), 0);

  Stats cur{}, prev{};
  auto last_time = std::chrono::steady_clock::now();

  while (g_keep_running) {
    ssize_t rx_size = recv_framed(connfd, rx_buf.data(), rx_buf.size());
    if (rx_size <= 0) break;
    cur.rx_count++;
    cur.rx_bytes += rx_size;

    // Copy the app header (window_slot) into the response.
    const app_hdr_t *req =
        reinterpret_cast<const app_hdr_t *>(rx_buf.data());
    app_hdr_t *resp = reinterpret_cast<app_hdr_t *>(tx_buf.data());
    resp->window_slot = req->window_slot;

    if (!send_framed(connfd, tx_buf.data(), g_msg_size)) break;
    cur.tx_count++;
    cur.tx_bytes += g_msg_size;

    report_stats(cur, prev, last_time);
  }

  std::cout << "[SERVER] Done. TX=" << cur.tx_count << " RX=" << cur.rx_count
            << std::endl;
  close(connfd);
  close(listenfd);
  return 0;
}

// ──────── Client mode ────────
// Connects to a remote server, sends a window of messages, then ping-pongs.

static int run_client() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return 1; }

  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_remote_port);
  inet_pton(AF_INET, g_remote_ip, &addr.sin_addr);

  std::cout << "[CLIENT] Connecting to " << g_remote_ip << ":"
            << g_remote_port << ", msg_size=" << g_msg_size
            << ", window=" << g_msg_window << std::endl;

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect"); return 1;
  }
  std::cout << "[CLIENT] Connected." << std::endl;

  std::vector<uint8_t> tx_buf(g_msg_size);
  std::vector<uint8_t> rx_buf(8 * 1024 * 1024);
  std::iota(tx_buf.begin(), tx_buf.end(), 0);

  Stats cur{}, prev{};
  auto last_time = std::chrono::steady_clock::now();

  // Send initial window.
  for (uint32_t i = 0; i < g_msg_window; i++) {
    app_hdr_t *hdr = reinterpret_cast<app_hdr_t *>(tx_buf.data());
    hdr->window_slot = i;
    if (!send_framed(fd, tx_buf.data(), g_msg_size)) {
      fprintf(stderr, "Failed to send initial window slot %u\n", i);
      return 1;
    }
    cur.tx_count++;
    cur.tx_bytes += g_msg_size;
  }

  // Steady-state: receive one, send one.
  while (g_keep_running) {
    ssize_t rx_size = recv_framed(fd, rx_buf.data(), rx_buf.size());
    if (rx_size <= 0) break;
    cur.rx_count++;
    cur.rx_bytes += rx_size;

    const app_hdr_t *resp =
        reinterpret_cast<const app_hdr_t *>(rx_buf.data());
    uint64_t slot = resp->window_slot;

    // Send next message for this slot.
    app_hdr_t *req = reinterpret_cast<app_hdr_t *>(tx_buf.data());
    req->window_slot = slot;
    if (!send_framed(fd, tx_buf.data(), g_msg_size)) break;
    cur.tx_count++;
    cur.tx_bytes += g_msg_size;

    report_stats(cur, prev, last_time);
  }

  std::cout << "[CLIENT] Done. TX=" << cur.tx_count << " RX=" << cur.rx_count
            << std::endl;
  close(fd);
  return 0;
}

// ──────── Main ────────

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s --local_ip=IP [--local_port=PORT] [--remote_ip=IP] "
          "[--remote_port=PORT] [--msg_size=N] [--msg_window=N]\n"
          "\n"
          "  If --remote_ip is given, runs as client; otherwise as server.\n",
          prog);
}

int main(int argc, char *argv[]) {
  signal(SIGINT, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  // Minimal flag parsing (--key=value style).
  for (int i = 1; i < argc; i++) {
    std::string arg(argv[i]);
    auto eq = arg.find('=');
    if (eq == std::string::npos) { usage(argv[0]); return 1; }
    std::string key = arg.substr(0, eq);
    std::string val = arg.substr(eq + 1);

    if (key == "--local_ip")       g_local_ip = strdup(val.c_str());
    else if (key == "--local_port")  g_local_port = atoi(val.c_str());
    else if (key == "--remote_ip")   g_remote_ip = strdup(val.c_str());
    else if (key == "--remote_port") g_remote_port = atoi(val.c_str());
    else if (key == "--msg_size")    g_msg_size = atoi(val.c_str());
    else if (key == "--msg_window")  g_msg_window = atoi(val.c_str());
    else { fprintf(stderr, "Unknown flag: %s\n", key.c_str()); return 1; }
  }

  if (g_msg_size < sizeof(app_hdr_t)) {
    fprintf(stderr, "msg_size must be >= %zu\n", sizeof(app_hdr_t));
    return 1;
  }

  if (g_remote_ip != nullptr) {
    return run_client();
  } else {
    return run_server();
  }
}
