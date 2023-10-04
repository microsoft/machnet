#include <benchmark/benchmark.h>
#include <glog/logging.h>
#include <jring2.h>
#include <utils.h>

#include <thread>

static constexpr size_t kProducerCore = 2;
static constexpr size_t kConsumerCore = 5;

static constexpr size_t kQueueSz = 1024;
static constexpr size_t kWindowSize = 16;

static constexpr size_t kMsgSize = 64;

class ProducerConsumerBenchmark : public benchmark::Fixture {
 public:
  ProducerConsumerBenchmark()
      : p2c_ring_(nullptr), c2p_ring_(nullptr), stop_(false) {}
  // This is the nconsume thread function.
  void consumer_task() {
    juggler::utils::BindThisThreadToCore(kConsumerCore);

    std::vector<uint8_t> buf(kMsgSize, 'k');
    std::vector<uint8_t> expected_buf(kMsgSize, 'a');
    while (!stop_.load()) {
      jring2_dequeue_burst(p2c_ring_, buf.data(), 1);
    }
  }

  void SetUp(const ::benchmark::State& state) override {
    // nInitialization code here...
    const auto kRingMemSz = jring2_get_buf_ring_size(kMsgSize, kQueueSz);

    // Allocate memory for the rings.
    p2c_ring_ = CHECK_NOTNULL(
        static_cast<jring2_t*>(aligned_alloc(CACHELINE_SIZE, kRingMemSz)));
    jring2_init(p2c_ring_, kQueueSz, kMsgSize);

    c2p_ring_ = CHECK_NOTNULL(
        static_cast<jring2_t*>(aligned_alloc(CACHELINE_SIZE, kRingMemSz)));
    jring2_init(c2p_ring_, kQueueSz, kMsgSize);

    // Start the consumer thread.
    stop_.store(false);
    consumer_ = std::thread(&ProducerConsumerBenchmark::consumer_task, this);
    consumer_.detach();
  }

  void TearDown(const ::benchmark::State& state) override {
    stop_.store(true);
    free(p2c_ring_);
    p2c_ring_ = nullptr;
    free(c2p_ring_);
    c2p_ring_ = nullptr;
  }

 protected:
  jring2_t* p2c_ring_{nullptr};
  jring2_t* c2p_ring_{nullptr};
  std::atomic<bool> stop_{false};
  std::thread consumer_{};
};

BENCHMARK_DEFINE_F(ProducerConsumerBenchmark, ProducerBenchmark)
(benchmark::State& st) {  // NOLINT
  // Give it a moment to start
  // Pause the timer to exclude the sleep from measurements
  st.PauseTiming();
  const uint32_t num_messages = st.range(0);
  juggler::utils::BindThisThreadToCore(kProducerCore);
  sleep(2);
  st.ResumeTiming();

  std::vector<uint8_t> tx_buf(kMsgSize, 'a');
  auto nb_tx = 0u;
  for (auto _ : st) {
    while (nb_tx < num_messages) {
      nb_tx += jring2_enqueue_bulk(p2c_ring_, tx_buf.data(), 1);
    }
  }
  st.counters["msg_rate"] =
      benchmark::Counter(nb_tx, benchmark::Counter::kIsRate);
  st.counters["bps"] =
      benchmark::Counter(nb_tx * kMsgSize, benchmark::Counter::kIsRate);
}

BENCHMARK_REGISTER_F(ProducerConsumerBenchmark, ProducerBenchmark)
    ->Range(1 << 20, 1 << 28);  // [1M, 16M]

BENCHMARK_MAIN();
