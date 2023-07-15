#include <glog/logging.h>

#include <cstdint>
#include <iostream>
#include <thread>

#include "jring.h"

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

jring_t *init_ring(size_t element_count) {
  size_t ring_sz = jring_get_buf_ring_size(sizeof(msg_t), element_count);
  LOG(INFO) << "Ring size: " << ring_sz << " bytes, msg size: " << sizeof(msg_t)
            << " bytes, element count: " << element_count;
  jring_t *ring = (jring_t *)malloc(ring_sz);
  if (ring == nullptr) {
    LOG(ERROR) << "Failed to allocate memory " << ring_sz
               << " bytes for ring buffer";
    exit(EXIT_FAILURE);
  }
  if (jring_init(ring, element_count, sizeof(msg_t), 1, 1) < 0) {
    LOG(ERROR) << "Failed to initialize ring buffer";
    exit(EXIT_FAILURE);
  }
  return ring;
}

void ProducerThread(jring_t *ring) {
  LOG(INFO) << "Producer thread started, binding to core "
            << kProducerCpuCoreId;
  BindThisThreadToCore(kProducerCpuCoreId);
  SetThisThreadName("jring_producer");

  size_t epoch = 0;
  while (true) {
    LOG(INFO) << "Producer epoch: " << epoch++ << ", will send " << kOpsPerEpoch
              << " messages";
    for (size_t i = 0; i < kOpsPerEpoch; ++i) {
      msg_t msg;
      clock_gettime(CLOCK_REALTIME, &msg.ts);
      while (jring_sp_enqueue_bulk(ring, &msg, 1, nullptr) != 1) {
        // do nothing
      }
      BusySleepNs(300);  // Assume 3 million packets/sec
    }
    BusySleepNs(1000 * 1000 * 1000);  // 1 sec
  }
}

void ConsumerThread(jring_t *ring) {
  LOG(INFO) << "Consumer thread started, binding to core "
            << kConsumerCpuCoreId;
  BindThisThreadToCore(kConsumerCpuCoreId);
  SetThisThreadName("jring_consumer");
  size_t epoch = 0;

  while (true) {
    size_t epoch_lat_sum_ns = 0;

    for (size_t i = 0; i < kOpsPerEpoch; ++i) {
      msg_t msg;
      while (jring_sc_dequeue_bulk(ring, &msg, 1, nullptr) != 1) {
        // do nothing
      }
      struct timespec end_time;
      clock_gettime(CLOCK_REALTIME, &end_time);
      long latency = (end_time.tv_sec - msg.ts.tv_sec) * 1e9 +
                     (end_time.tv_nsec - msg.ts.tv_nsec);
      epoch_lat_sum_ns += latency;
    }

    LOG(INFO) << "Consumer epoch: " << epoch++
              << " avg latency: " << epoch_lat_sum_ns / kOpsPerEpoch
              << " ns over " << kOpsPerEpoch << " messages";
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
