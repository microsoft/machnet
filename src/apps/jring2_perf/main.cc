
#include <bits/stdc++.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <utils.h>

#include "jring2.h"

static constexpr size_t kProducerCore = 2;
static constexpr size_t kConsumerCore = 7;

static constexpr size_t kQueueSz = 1024;
static constexpr size_t kWindowSize = 16;

static constexpr size_t kMsgPayloadSize8B = 7;

std::atomic<bool> g_stop{false};

struct Msg {
  // Constructor to initialize the message to zero.
  Msg() { memset(this, 0, sizeof(Msg)); }
  size_t val[kMsgPayloadSize8B];
};

jring2_t *g_p2c_ring;  // Producer to consumer ring
jring2_t *g_c2p_ring;  // Consumer to producer ring
double g_rdtsc_freq_ghz = 0.0;

uint64_t rdtsc() {
  uint32_t lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

double MeasureRdtscFreqGHz() {
  const uint64_t start_rdtsc = rdtsc();
  const uint64_t start_ns = time(NULL) * 1000000000;
  sleep(1);
  const uint64_t end_rdtsc = rdtsc();
  const uint64_t end_ns = time(NULL) * 1000000000;

  const uint64_t rdtsc_diff = end_rdtsc - start_rdtsc;
  const uint64_t ns_diff = end_ns - start_ns;

  return rdtsc_diff * 1.0 / ns_diff;
}

void ProducerThread() {
  juggler::utils::BindThisThreadToCore(kProducerCore);

  size_t seq_num = 0;
  size_t sum_lat_cycles = 0;
  uint64_t msr_start_cycles = rdtsc();
  srand(time(NULL));

  std::array<uint64_t, kWindowSize> timestamps{};
  size_t inflight_requests = 0;

  while (!g_stop.load()) {
    // If there are kWindowSize outstanding requests, wait for a response
    if (inflight_requests == kWindowSize) {
      Msg resp_msg{};
      while (true) {
        int result = jring2_dequeue_burst(g_c2p_ring, &resp_msg, 1);
        if (result == 1) break;
      }

      const size_t msg_lat_cycles =
          (rdtsc() - timestamps[seq_num % kWindowSize]);
      sum_lat_cycles += msg_lat_cycles;

      // Check message contents
      for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
        if (resp_msg.val[i] != seq_num) {
          std::cerr << "Producer error: val mismatch, expected: " << seq_num
                    << " actual(" << i << "): " << resp_msg.val[i] << std::endl;
          exit(1);
        }
      }

      seq_num++;
      inflight_requests--;

      // Print once every million msg
      static constexpr size_t kPrintEveryNMsgs = (1024 * 1024);
      if (seq_num % kPrintEveryNMsgs == 0) {
        const size_t avg_lat_cycles = sum_lat_cycles / 1000000;
        const size_t avg_lat_ns = avg_lat_cycles / g_rdtsc_freq_ghz;

        const size_t ns_since_msr =
            (rdtsc() - msr_start_cycles) / g_rdtsc_freq_ghz;
        const double us_since_msr = ns_since_msr / 1000.0;

        printf("Producer: avg RTT latency for %lu byte msgs: %lu ns\n",
               sizeof(Msg), avg_lat_ns);
        printf("Producer: Msg rate: %.2f M reqs/sec (%.2f M msgs/sec)\n",
               kPrintEveryNMsgs / us_since_msr,
               kPrintEveryNMsgs * 2 / us_since_msr);

        msr_start_cycles = rdtsc();
        sum_lat_cycles = 0;
      }
    }

    // Issue a new request
    const uint64_t req_rdtsc = rdtsc();
    Msg req_msg{};
    for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
      req_msg.val[i] = seq_num + inflight_requests;
    }

    timestamps[(seq_num + inflight_requests) % kWindowSize] = req_rdtsc;
    jring2_enqueue_bulk(g_p2c_ring, &req_msg, 1);
    inflight_requests++;
  }
  std::cout << "Producer exiting" << std::endl;
  exit(0);
}

void ConsumerThread() {
  juggler::utils::BindThisThreadToCore(kConsumerCore);

  while (!g_stop.load()) {
    Msg req_msg{};
    while (true) {
      int result = jring2_dequeue_burst(g_p2c_ring, &req_msg, 1);
      if (result == 1) break;
    }

    // Send a response
    Msg resp_msg{};
    for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
      resp_msg.val[i] = req_msg.val[i];
    }

    auto ret = jring2_enqueue_bulk(g_c2p_ring, &resp_msg, 1);
    if (ret != 1) {
      std::cerr << "Consumer error: enqueue failed" << std::endl;
      exit(1);
    }
  }

  std::cout << "Consumer exiting" << std::endl;
  exit(0);
}

int main() {
  std::cout << "Measuring RDTSC freq" << std::endl;
  g_rdtsc_freq_ghz = MeasureRdtscFreqGHz();
  std::cout << "RDTSC freq: " << g_rdtsc_freq_ghz << " GHz" << std::endl;
  if (g_rdtsc_freq_ghz < 1.0 || g_rdtsc_freq_ghz > 4.0) {
    std::cerr << "ERROR: RDTSC freq is too high or too low" << std::endl;
    exit(1);
  }

  // Register signal handler.
  signal(SIGINT, [](int) { g_stop.store(true); });

  // Initialize
  const auto kRingMemSz = jring2_get_buf_ring_size(sizeof(Msg), kQueueSz);
  if (kRingMemSz == -1ull) {
    std::cerr << "ERROR: jring2_get_buf_ring_size failed" << std::endl;
    exit(1);
  }
  g_p2c_ring =
      static_cast<jring2_t *>(aligned_alloc(CACHELINE_SIZE, kRingMemSz));
  if (g_p2c_ring == nullptr) {
    std::cerr << "ERROR: aligned_alloc failed (g_p2c_ring)" << std::endl;
    exit(1);
  }

  g_c2p_ring =
      static_cast<jring2_t *>(aligned_alloc(CACHELINE_SIZE, kRingMemSz));
  if (g_c2p_ring == nullptr) {
    std::cerr << "ERROR: aligned_alloc failed (g_c2p_ring)" << std::endl;
    exit(1);
  }

  jring2_init(g_p2c_ring, kQueueSz, sizeof(Msg));
  jring2_init(g_c2p_ring, kQueueSz, sizeof(Msg));

  std::cout << "Starting consumer thread" << std::endl;
  std::thread trecv(ConsumerThread);

  sleep(1);
  std::cout << "Starting producer thread" << std::endl;
  std::thread tsend(ProducerThread);

  tsend.join();
  trecv.join();

  free(g_p2c_ring);
  free(g_c2p_ring);

  return 0;
}
