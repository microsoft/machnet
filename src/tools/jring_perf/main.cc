#include <common.h>
#include <glog/logging.h>
#include <jring.h>
#include <utils.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

static constexpr size_t kNumRingElems = (1024 * 64);
static constexpr size_t kMsgSize = 128;
static constexpr size_t kOpsPerEpoch = kNumRingElems;
static constexpr size_t kProducerCpuCoreId = 2;
static constexpr size_t kConsumerCpuCoreId = 5;

// Message struct for queue.
struct msg_t {
  struct timespec ts;
  char data[kMsgSize - sizeof(struct timespec)];
};
static_assert(sizeof(msg_t) == kMsgSize, "Message size is not correct");

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

jring_t *init_ring(size_t element_count) {
  size_t ring_sz = jring_get_buf_ring_size(sizeof(msg_t), element_count);
  LOG(INFO) << "Ring size: " << ring_sz << " bytes, msg size: " << sizeof(msg_t)
            << " bytes, element count: " << element_count;
  jring_t *ring = CHECK_NOTNULL(reinterpret_cast<jring_t *>(aligned_alloc(
      juggler::hardware_constructive_interference_size, ring_sz)));
  if (jring_init(ring, element_count, sizeof(msg_t), 1, 1) < 0) {
    LOG(ERROR) << "Failed to initialize ring buffer";
    free(ring);
    exit(EXIT_FAILURE);
  }
  return ring;
}

void ProducerThread(jring_t *ring) {
  LOG(INFO) << "Producer thread started, binding to core "
            << kProducerCpuCoreId;
  juggler::utils::BindThisThreadToCore(kProducerCpuCoreId);
  SetThisThreadName("jring_producer");

  struct timespec msr_start;
  clock_gettime(CLOCK_REALTIME, &msr_start);
  size_t num_msg_since_last_msr = 0;

  while (true) {
    msg_t msg;
    clock_gettime(CLOCK_REALTIME, &msg.ts);
    while (jring_sp_enqueue_bulk(ring, &msg, 1, nullptr) != 1) {
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

void ConsumerThread(jring_t *ring) {
  LOG(INFO) << "Consumer thread started, binding to core "
            << kConsumerCpuCoreId;
  juggler::utils::BindThisThreadToCore(kConsumerCpuCoreId);
  SetThisThreadName("jring_consumer");

  struct timespec msr_start;
  clock_gettime(CLOCK_REALTIME, &msr_start);
  size_t num_rx = 0;
  size_t ns_sum = 0;

  while (true) {
    msg_t msg;
    while (jring_sc_dequeue_bulk(ring, &msg, 1, nullptr) != 1) {
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
  jring_t *ring = init_ring(kNumRingElems);
  std::thread producer(ProducerThread, ring);
  std::thread consumer(ConsumerThread, ring);

  producer.join();
  consumer.join();

  free(ring);
  return 0;
}
