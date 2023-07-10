/**
 * @file buffer_ring_test.cc
 *
 * Unit tests for juggler's ring library.
 */

#include <buffer_ring.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <utils.h>

#include <future>
#include <memory>
#include <numeric>
#include <random>
#include <thread>

TEST(RingInit, RingInit1) {
  using juggler::ConsumerType;
  using juggler::ProducerType;
  const size_t kMaxSize = 4096;
  const size_t kElementSize = sizeof(uintptr_t);
  std::vector<size_t> nslots(kMaxSize, 0);

  std::iota(nslots.begin(), nslots.end(), 1);

  // Allocate enough space for the ring control block and slots, to facilitate
  // all testing sizes.
  void *map = malloc(sizeof(jring) + kMaxSize * sizeof(void *));
  CHECK_NOTNULL(map);
  for (auto &capacity : nslots) {
    auto *ring = reinterpret_cast<juggler::BufferRing<
        ProducerType::kSingleProducer, ConsumerType::kSingleConsumer> *>(map);
    auto ret = ring->Init(capacity, kElementSize);

    // Rings initialization is only successful when number of slots is power
    // of two.
    if (juggler::utils::is_power_of_two(capacity))
      EXPECT_TRUE(ret) << "Failed with capacity: " << capacity;
    else
      EXPECT_FALSE(ret) << "Failed with capacity: " << capacity;
  }
  free(map);
}

TEST(SpScRingTest, SpScRingTest) {
  using juggler::ConsumerType;
  using juggler::ProducerType;

  const size_t kMaxSize = 2048;
  const size_t kElementSize = sizeof(uintptr_t);
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist(0, UINT64_MAX);

  uint64_t objtable[kMaxSize];
  for (size_t i = 0; i < kMaxSize; i++) objtable[i] = dist(rng);

  // Allocate enough space for the ring control block and slots, to facilitate
  // all testing sizes.
  auto map = std::make_unique<unsigned char[]>(sizeof(jring) +
                                               kMaxSize * sizeof(void *));
  CHECK_NOTNULL(map);
  auto *spsc_ring =
      reinterpret_cast<juggler::BufferRing<ProducerType::kSingleProducer,
                                           ConsumerType::kSingleConsumer> *>(
          map.get());

  // Initialize the ring.
  EXPECT_TRUE(spsc_ring->Init(kMaxSize, kElementSize));

  // Test enqueues and dequeues.
  std::vector<unsigned int> enqueue_batch_sizes(kMaxSize, 0);
  std::iota(enqueue_batch_sizes.begin(), enqueue_batch_sizes.end(), 0);
  for (auto &batch_size : enqueue_batch_sizes) {
    // Enqueue the objects.
    unsigned int free_space;
    unsigned int enqueued = spsc_ring->Enqueue(
        reinterpret_cast<void *const *>(objtable), batch_size, &free_space);
    EXPECT_EQ(enqueued, batch_size);
    EXPECT_EQ(free_space, kMaxSize - enqueued - 1);

    // Dequeue the objects in smaller batches, if possible.
    unsigned int dequeue_batch_size = batch_size / 2;
    if (dequeue_batch_size == 0) dequeue_batch_size = 1;
    uint64_t dequeued_objtable[kMaxSize];
    do {
      dequeue_batch_size = std::min(dequeue_batch_size, enqueued);
      unsigned int available;
      unsigned int dequeued = spsc_ring->Dequeue(
          reinterpret_cast<void **>(dequeued_objtable + batch_size - enqueued),
          dequeue_batch_size, &available);
      EXPECT_EQ(dequeued, dequeue_batch_size);
      enqueued -= dequeued;
      EXPECT_EQ(available, enqueued);
    } while (enqueued > 0);

    // Check that the dequeued objects are the same as the enqueued ones.
    for (size_t i = 0; i < batch_size; i++) {
      EXPECT_EQ(objtable[i], dequeued_objtable[i]);
    }
  }
}

TEST(SpScRingTest, SpScRingTest2) {
  using juggler::ConsumerType;
  using juggler::ProducerType;

  const size_t kMaxSize = 2048;
  const size_t kElementSize = sizeof(uintptr_t);

  // Allocate enough space for the ring control block and slots, to facilitate
  // all testing sizes.
  auto map = std::make_unique<unsigned char[]>(sizeof(jring) +
                                               kMaxSize * sizeof(void *));
  CHECK_NOTNULL(map);
  auto *spsc_ring =
      reinterpret_cast<juggler::BufferRing<ProducerType::kSingleProducer,
                                           ConsumerType::kSingleConsumer> *>(
          map.get());

  // Initialize the ring.
  EXPECT_TRUE(spsc_ring->Init(kMaxSize, kElementSize));

  /**
   * This is the producer lambda implementation.
   *
   * @param ring  Opaque pointer to the SPSC ring to consume from.
   * @param num_elems Total number of elements expected to be dequed.
   * @param should_terminate  A flag to trigger termination.
   */
  auto producer = [](juggler::BufferRing<ProducerType::kSingleProducer,
                                         ConsumerType::kSingleConsumer> *ring,
                     size_t num_elems,
                     const std::atomic<bool> &should_terminate) {
    static size_t counter;

    while (num_elems) {
      if (should_terminate.load()) break;
      size_t enqueued =
          ring->Enqueue(reinterpret_cast<void *const *>(&counter), 1);
      if (enqueued == 0) continue;  // Ring is full.
      counter++;
      num_elems--;
    }
  };

  /**
   * This is the consumer lambda implementation.
   *
   * @param ring  Opaque pointer to the SPSC ring to consume from.
   * @param num_elems Total number of elements expected to be dequed.
   * @param p  A promise to set success or failure.
   * @param should_terminate  A flag to trigger termination.
   */
  auto consumer = [](juggler::BufferRing<ProducerType::kSingleProducer,
                                         ConsumerType::kSingleConsumer> *ring,
                     size_t num_elems, std::promise<int> p,
                     const std::atomic<bool> &should_terminate) {
    static size_t counter;

    while (num_elems) {
      if (should_terminate.load()) break;

      size_t received_elem;
      size_t dequeued =
          ring->Dequeue(reinterpret_cast<void **>(&received_elem), 1);
      if (dequeued == 0) continue;            // Ring is empty.
      if (received_elem != counter++) break;  // Error -- unexpected element.
      num_elems--;
    }

    // On success num_elems should be zero.
    p.set_value_at_thread_exit(num_elems);
  };

  // Number of elements to transfer.
  const size_t num_elems = 16 * kMaxSize;

  std::atomic<bool> terminate_flag(false);

  // Make a future for the consumer and launch it in a thread.
  std::promise<int> p;
  std::future<int> consumer_completes = p.get_future();
  std::thread(consumer, spsc_ring, num_elems, std::move(p),
              std::ref(terminate_flag))
      .detach();

  // Launch the producer.
  std::thread(producer, spsc_ring, num_elems, std::ref(terminate_flag))
      .detach();

  // Wait for consumer to complete.
  if (std::future_status::ready ==
      consumer_completes.wait_for(std::chrono::seconds(1))) {
    auto consumer_retval = consumer_completes.get();
    if (consumer_retval == -1) {
      // Consumer encountered an error to dequeued elements.
      // Try to terminate the producer.
      terminate_flag.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // We expect the future value to be 0, otherwise an error occured.
    EXPECT_EQ(consumer_retval, 0);
  } else {
    // Time ran out and threads haven't completed, so we set the termination
    // flag.
    terminate_flag.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    FAIL() << "Timeout. Threads never completed.";
  }
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);

  int ret = RUN_ALL_TESTS();
  return ret;
}
