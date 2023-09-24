#include <glog/logging.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "jring2.h"

static constexpr size_t kNumRingElems = (1024 * 64);
static constexpr size_t kMsgSize = 128;
static constexpr size_t kOpsPerEpoch = kNumRingElems;
static constexpr size_t kProducerCpuCoreId = 2;
static constexpr size_t kConsumerCpuCoreId = 3;

// Message struct for queue.
struct msg_t {
  struct timespec ts;
  char data[kMsgSize - sizeof(struct timespec)];
};
static_assert(sizeof(msg_t) == kMsgSize, "Message size is not correct");

void BindThisThreadToCore(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void SetThisThreadName(const std::string &name) {
  pthread_setname_np(pthread_self(), name.c_str());
}

void BusySleepNs(size_t nsec) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  size_t start_ns = ts.tv_sec * 1000000000 + ts.tv_nsec;
  while (true) {
    clock_gettime(CLOCK_REALTIME, &ts);
    size_t now_ns = ts.tv_sec * 1000000000 + ts.tv_nsec;
    if (now_ns - start_ns >= nsec) break;
  }
}

jring2_t *init_ring(size_t element_count) {
  size_t ring_sz = jring2_get_buf_ring_size(sizeof(msg_t), element_count);
  LOG(INFO) << "Ring size: " << ring_sz << " bytes, msg size: " << sizeof(msg_t)
            << " bytes, element count: " << element_count;
  jring2_t *ring = reinterpret_cast<jring2_t *>(malloc(ring_sz));
  if (ring == nullptr) {
    LOG(ERROR) << "Failed to allocate memory " << ring_sz
               << " bytes for ring buffer";
    exit(EXIT_FAILURE);
  }
  if (jring2_init(ring, element_count, sizeof(msg_t)) < 0) {
    LOG(ERROR) << "Failed to initialize ring buffer";
    exit(EXIT_FAILURE);
  }
  return ring;
}

void ProducerThread(jring2_t *ring) {
  LOG(INFO) << "Producer thread started, binding to core "
            << kProducerCpuCoreId;
  BindThisThreadToCore(kProducerCpuCoreId);
  SetThisThreadName("jring_producer");

  struct timespec msr_start;
  clock_gettime(CLOCK_REALTIME, &msr_start);
  size_t num_msg_since_last_msr = 0;

  while (true) {
    msg_t msg;
    clock_gettime(CLOCK_REALTIME, &msg.ts);
    while (jring2_enqueue_bulk(ring, &msg, 1) != 1) {
      // do nothing
    }

    BusySleepNs(500);  // Emulate 2 Mpps

    // check if 1 sec has elapsed since last msr, using msg.ts
    const size_t ns_since_last_msr = (msg.ts.tv_sec - msr_start.tv_sec) * 1e9 +
                                     (msg.ts.tv_nsec - msr_start.tv_nsec);
    if (ns_since_last_msr >= 1e9) {
      const size_t kpps = num_msg_since_last_msr / 1e3;
      LOG(INFO) << "Producer: " << kpps << " Kpps";
      num_msg_since_last_msr = 0;
      msr_start = msg.ts;
    } else {
      ++num_msg_since_last_msr;
    }
  }
}

void ConsumerThread(jring2_t *ring) {
  LOG(INFO) << "Consumer thread started, binding to core "
            << kConsumerCpuCoreId;
  BindThisThreadToCore(kConsumerCpuCoreId);
  SetThisThreadName("jring_consumer");

  struct timespec msr_start;
  clock_gettime(CLOCK_REALTIME, &msr_start);
  size_t num_rx = 0;
  size_t ns_sum = 0;

  while (true) {
    msg_t msg;
    while (jring2_dequeue_burst(ring, &msg, 1) != 1) {
      // do nothing
    }
    num_rx++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    const size_t msg_lat_ns =
        (ts.tv_sec - msg.ts.tv_sec) * 1e9 + (ts.tv_nsec - msg.ts.tv_nsec);
    ns_sum += msg_lat_ns;

    const size_t ns_since_last_msr =
        (ts.tv_sec - msr_start.tv_sec) * 1e9 + (ts.tv_nsec - msr_start.tv_nsec);
    if (ns_since_last_msr >= 1e9) {
      const double kpps = num_rx / 1e3;
      const size_t avg_lat_ns = ns_sum / num_rx;
      LOG(INFO) << "Consumer: " << kpps << " Kpps, avg latency: " << avg_lat_ns
                << " ns";
      num_rx = 0;
      ns_sum = 0;
      msr_start = ts;
    }
  }
}

int main() {
  google::InitGoogleLogging("jring_bench");
  FLAGS_logtostderr = true;
  jring2_t *ring = init_ring(kNumRingElems);
  std::thread producer(ProducerThread, ring);
  std::thread consumer(ConsumerThread, ring);

  producer.join();
  consumer.join();

  free(ring);
  return 0;
}
