#include <benchmark/benchmark.h>
#include <glog/logging.h>
#include <jring2.h>
#include <utils.h>

#include <thread>

static constexpr size_t kProducerCore = 2;
static constexpr size_t kConsumerCore = 5;

static constexpr size_t kQueueSz = 1024;
static constexpr size_t KMaxMsgSize = 2048;

class ProducerConsumerBenchmark : public benchmark::Fixture {
 public:
  ProducerConsumerBenchmark()
      : p2c_ring_(nullptr), c2p_ring_(nullptr), stop_(false) {}
  /**
   * @brief Consumer task.
   */
  void consumer_task() {
    juggler::utils::BindThisThreadToCore(kConsumerCore);

    std::vector<uint8_t> buf(KMaxMsgSize);
    auto nb_rx = 0u;
    while (!stop_.load()) {
      nb_rx += jring2_dequeue_burst(p2c_ring_, buf.data(), 1);
    }
    (void)nb_rx;
  }

  void SetUp(const ::benchmark::State& state) override {
    // Initialize the message size from the benchmark state.
    const size_t msg_size = static_cast<size_t>(state.range(0));
    const auto ring_mem_size = jring2_get_buf_ring_size(msg_size, kQueueSz);

    // Allocate memory for the rings.
    p2c_ring_ = CHECK_NOTNULL(
        static_cast<jring2_t*>(aligned_alloc(CACHELINE_SIZE, ring_mem_size)));
    jring2_init(p2c_ring_, kQueueSz, msg_size);

    c2p_ring_ = CHECK_NOTNULL(
        static_cast<jring2_t*>(aligned_alloc(CACHELINE_SIZE, ring_mem_size)));
    jring2_init(c2p_ring_, kQueueSz, msg_size);

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
  const size_t msg_size = static_cast<size_t>(st.range(0));
  const uint32_t num_messages = static_cast<uint32_t>(st.range(1));
  CHECK_GE(msg_size, sizeof(uint64_t));

  juggler::utils::BindThisThreadToCore(kProducerCore);
  sleep(2);

  std::vector<uint8_t> tx_buf(msg_size, 'a');
  auto nb_tx = 0u;
  for (auto _ : st) {
    auto iteration_nb_tx = 0u;
    while (iteration_nb_tx < num_messages) {
      ::benchmark::DoNotOptimize(tx_buf);
      ::benchmark::DoNotOptimize(
          iteration_nb_tx += jring2_enqueue_bulk(p2c_ring_, tx_buf.data(), 1));
    }
    nb_tx += iteration_nb_tx;
  }

  st.counters["msg_rate"] =
      benchmark::Counter(nb_tx, benchmark::Counter::kIsRate);
  st.counters["bps"] =
      benchmark::Counter(nb_tx * msg_size * 8, benchmark::Counter::kIsRate);
}

BENCHMARK_REGISTER_F(ProducerConsumerBenchmark, ProducerBenchmark)
    ->Args({64, 1 << 24})    // msg_size = 64, num_messages = 16M
    ->Args({128, 1 << 24})   // msg_size = 128, num_messages = 16M
    ->Args({256, 1 << 24})   // msg_size = 256, num_messages = 16M
    ->Args({512, 1 << 24})   // msg_size = 512, num_messages = 16M
    ->Args({1024, 1 << 24})  // msg_size = 1024, num_messages = 16M
    ->Args({2048, 1 << 24})  // msg_size = 1024, num_messages = 16M
    ->Iterations(10);  // number of iterations for each case

BENCHMARK_MAIN();
