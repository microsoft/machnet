#ifndef SRC_INCLUDE_BUFFER_RING_H_
#define SRC_INCLUDE_BUFFER_RING_H_

#include <common.h>
#include <jring.h>
#include <utils.h>

namespace juggler {

enum class ProducerType : int { kSingleProducer = 0, kMultiProducer = 1 };
enum class ConsumerType : int { kSingleConsumer = 0, kMultiConsumer = 1 };

template <ProducerType IsMultiProd, ConsumerType IsMultiCons>
class alignas(juggler::hardware_constructive_interference_size) BufferRing {
 public:
  BufferRing() = delete;

  bool Init(uint32_t capacity, uint32_t element_size, ProducerType isMultiProd,
            ConsumerType isMultiCons) {
    int ret = jring_init(&ring_, capacity, element_size,
                         static_cast<int>(IsMultiProd),
                         static_cast<int>(IsMultiCons));
    if (ret < 0) {
      LOG(WARNING) << "Failed to initialize ring.";
      return false;
    }

    return true;
  }

 private:
  struct jring ring_;
};

template <>
class alignas(juggler::hardware_constructive_interference_size)
    BufferRing<ProducerType::kSingleProducer, ConsumerType::kSingleConsumer> {
 public:
  BufferRing() = delete;

  static uint32_t CalculateSize(uint32_t capacity, uint32_t element_size) {
    return jring_get_buf_ring_size(capacity, element_size);
  }

  bool Init(uint32_t capacity, uint32_t element_size) {
    int ret = jring_init(&ring_, capacity, element_size,
                         static_cast<int>(ProducerType::kSingleProducer),
                         static_cast<int>(ConsumerType::kSingleConsumer));
    if (ret < 0) {
      LOG(WARNING) << "Failed to initialize ring.";
      return false;
    }

    return true;
  }

  uint32_t GetCapacity() const { return ring_.capacity; }

  bool IsFull() const { return static_cast<bool>(jring_full(&ring_)); }

  unsigned int Enqueue(void *const *items, unsigned int num_items,
                       unsigned int *free_space = nullptr) {
    return jring_sp_enqueue_bulk(&ring_, items, num_items, free_space);
  }
  unsigned int EnqueueBurst(void *const *items, unsigned int num_items,
                            unsigned int *free_space = nullptr) {
    return jring_sp_enqueue_burst(&ring_, items, num_items, free_space);
  }
  unsigned int Dequeue(void **items, unsigned int num_items,
                       unsigned int *available = nullptr) {
    return jring_sc_dequeue_bulk(&ring_, items, num_items, available);
  }
  unsigned int DequeueBurst(void **items, unsigned int num_items,
                            unsigned int *available = nullptr) {
    return jring_sc_dequeue_burst(&ring_, items, num_items, available);
  }

 private:
  struct jring ring_;
};

template <>
class alignas(juggler::hardware_constructive_interference_size)
    BufferRing<ProducerType::kMultiProducer, ConsumerType::kSingleConsumer> {
 public:
  BufferRing() = delete;

  bool Init(uint32_t capacity, uint32_t element_size) {
    int ret = jring_init(&ring_, capacity, element_size,
                         static_cast<int>(ProducerType::kMultiProducer),
                         static_cast<int>(ConsumerType::kSingleConsumer));
    if (ret < 0) {
      LOG(WARNING) << "Failed to initialize ring.";
      return false;
    }

    return true;
  }

  uint32_t GetCapacity() const { return ring_.capacity; }

  unsigned int Enqueue(void *const *items, unsigned int num_items,
                       unsigned int *free_space = nullptr) {
    return jring_mp_enqueue_bulk(&ring_, items, num_items, free_space);
  }
  unsigned int EnqueueBurst(void *const *items, unsigned int num_items,
                            unsigned int *free_space = nullptr) {
    return jring_mp_enqueue_burst(&ring_, items, num_items, free_space);
  }
  unsigned int Dequeue(void **items, unsigned int num_items,
                       unsigned int *available = nullptr) {
    return jring_sc_dequeue_bulk(&ring_, items, num_items, available);
  }
  unsigned int DequeueBurst(void **items, unsigned int num_items,
                            unsigned int *available = nullptr) {
    return jring_sc_dequeue_burst(&ring_, items, num_items, available);
  }

 private:
  struct jring ring_;
};

template <>
class BufferRing<ProducerType::kSingleProducer, ConsumerType::kMultiConsumer> {
 public:
  BufferRing() = delete;

  bool Init(uint32_t capacity, uint32_t element_size) {
    int ret = jring_init(&ring_, capacity, element_size,
                         static_cast<int>(ProducerType::kSingleProducer),
                         static_cast<int>(ConsumerType::kMultiConsumer));
    if (ret < 0) {
      LOG(WARNING) << "Failed to initialize ring.";
      return false;
    }

    return true;
  }

  uint32_t GetCapacity() const { return ring_.capacity; }

  unsigned int Enqueue(void *const *items, unsigned int num_items,
                       unsigned int *free_space = nullptr) {
    return jring_sp_enqueue_bulk(&ring_, items, num_items, free_space);
  }
  unsigned int EnqueueBurst(void *const *items, unsigned int num_items,
                            unsigned int *free_space = nullptr) {
    return jring_sp_enqueue_burst(&ring_, items, num_items, free_space);
  }
  unsigned int Dequeue(void **items, unsigned int num_items,
                       unsigned int *available = nullptr) {
    return jring_mc_dequeue_bulk(&ring_, items, num_items, available);
  }
  unsigned int DequeueBurst(void **items, unsigned int num_items,
                            unsigned int *available = nullptr) {
    return jring_mc_dequeue_burst(&ring_, items, num_items, available);
  }

 private:
  struct jring ring_;
};

};  // namespace juggler

#endif  // SRC_INCLUDE_BUFFER_RING_H_
