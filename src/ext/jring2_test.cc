
#include "jring2.h"

#include <bits/stdc++.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

static constexpr size_t kProducerCore = 2;
static constexpr size_t kConsumerCore = 3;

static constexpr size_t kQueueSz = 1024;
static constexpr size_t kWindowSize = 1;

static constexpr size_t kMsgPayloadSize8B = 7;
struct Msg {
  size_t val[kMsgPayloadSize8B];
};
static_assert(sizeof(jring2_ent_t::data) == 56,
              "jring2_ent_t::data is not 56 bytes");
static_assert(sizeof(Msg) <= sizeof(jring2_ent_t::data),
              "Msg is larger than jring2_ent_t::data");

jring2_t g_p2c_ring;  // Producer to consumer ring
jring2_t g_c2p_ring;  // Consumer to producer ring
double g_rdtsc_freq_ghz = 0.0;

void BindThisThreadToCore(size_t core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

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
  BindThisThreadToCore(kProducerCore);

  size_t seq_num = 0;
  size_t sum_lat_cycles = 0;
  uint64_t msr_start_cycles = rdtsc();
  srand(time(NULL));

  std::array<uint64_t, kWindowSize> timestamps;
  size_t inflight_requests = 0;

  while (true) {
    // If there are kWindowSize outstanding requests, wait for a response
    if (inflight_requests == kWindowSize) {
      jring2_ent_t resp_msg;
      while (true) {
        int result = jring2_dequeue(&g_c2p_ring, &resp_msg);
        if (result == 0) break;
      }

      const size_t msg_lat_cycles =
          (rdtsc() - timestamps[seq_num % kWindowSize]);
      sum_lat_cycles += msg_lat_cycles;

      // Check message contents
      const auto* data = reinterpret_cast<Msg*>(resp_msg.data);
      for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
        if (data->val[i] != seq_num) {
          std::cerr << "Producer error: val mismatch, expected: " << seq_num
                    << " actual: " << data->val[i] << std::endl;
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
    jring2_ent_t req_ent;
    auto* req_msg = reinterpret_cast<Msg*>(req_ent.data);
    for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
      req_msg->val[i] = seq_num + inflight_requests;
    }

    timestamps[(seq_num + inflight_requests) % kWindowSize] = req_rdtsc;
    jring2_enqueue(&g_p2c_ring, &req_ent);
    inflight_requests++;
  }
}

void ConsumerThread() {
  BindThisThreadToCore(kConsumerCore);

  while (true) {
    jring2_ent_t req_ent;
    while (true) {
      int result = jring2_dequeue(&g_p2c_ring, &req_ent);
      if (result == 0) break;
    }

    // Send a response
    jring2_ent_t resp_ent;
    const auto* req_msg = reinterpret_cast<Msg*>(req_ent.data);
    auto* resp_msg = reinterpret_cast<Msg*>(resp_ent.data);

    if (!resp_msg) {
      std::cerr << "ERROR: g_c2p_ring is full" << std::endl;
      exit(1);
    }
    for (size_t i = 0; i < kMsgPayloadSize8B; i++) {
      resp_msg->val[i] = req_msg->val[i];
    }

    jring2_enqueue(&g_c2p_ring, &resp_ent);
  }
}

int main() {
  std::cout << "Measuring RDTSC freq" << std::endl;
  g_rdtsc_freq_ghz = MeasureRdtscFreqGHz();
  std::cout << "RDTSC freq: " << g_rdtsc_freq_ghz << " GHz" << std::endl;
  if (g_rdtsc_freq_ghz < 1.0 || g_rdtsc_freq_ghz > 4.0) {
    std::cerr << "ERROR: RDTSC freq is too high or too low" << std::endl;
    exit(1);
  }

  jring2_init(&g_p2c_ring, kQueueSz);
  jring2_init(&g_c2p_ring, kQueueSz);

  std::cout << "Starting consumer thread" << std::endl;
  std::thread trecv(ConsumerThread);

  sleep(1);
  std::cout << "Starting producer thread" << std::endl;
  std::thread tsend(ProducerThread);

  tsend.join();
  trecv.join();

  jring2_deinit(&g_p2c_ring);
  jring2_deinit(&g_c2p_ring);

  return 0;
}
