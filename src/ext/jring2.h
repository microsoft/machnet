/*
MIT License

Copyright (c) 2018 Meng Rao <raomeng1@gmail.com>
Copyright (c) 2023 Anuj Kalia<ankalia@microsoft.com>
Copyright (c) 2023 Ilias Marinos <ilias@marinos.io>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef SRC_EXT_JRING2_H_
#define SRC_EXT_JRING2_H_

/**
 * @file A fast SPSC ring implementation.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CACHELINE_SIZE 64

#define ISPOWEROF2(x) (((((x)-1) & (x)) == 0) && x)
#define __ROUND_MASK(x, y) ((__typeof__(x))((y)-1))
#define ALIGN_UP_POW2(x, y) ((((x)-1) | __ROUND_MASK(x, y)) + 1)
#define ROUND_DOWN_POW2(x, y) ((x) & ~__ROUND_MASK(x, y))

typedef struct {
  uint32_t dd;  // Descriptor done.
  uint8_t data[0];
} jring2_entry_t;
static_assert(sizeof(jring2_entry_t) == 4);

/// @brief SPSC ring buffer.
typedef struct {
  uint32_t cnt;
  uint32_t mask;
  uint32_t element_size;  // Size of each object stored in the ring.
  uint32_t slot_size;     // element_size + sizeof(jring2_entry_t)

  uint8_t pad0 __attribute__((aligned(CACHELINE_SIZE)));
  uint64_t write_idx;  // Used only by writing thread
  // Number of slots the writer can write to without overwriting unread entries
  uint64_t free_write_cnt;

  uint8_t pad1 __attribute__((aligned(CACHELINE_SIZE)));
  volatile uint64_t read_idx;  // Used by both writing and reading thread

  uint8_t pad2 __attribute__((aligned(CACHELINE_SIZE)));
} jring2_t;
static_assert(sizeof(jring2_t) % CACHELINE_SIZE == 0,
              "jring2_t must be cache line aligned");

static __attribute__((always_inline)) inline jring2_entry_t *__jring2_get_slot(
    jring2_t *ring, const uint32_t idx) {
  assert(idx < ring->cnt);
  uint8_t *ring_slots = (uint8_t *)(ring + 1);
  return (jring2_entry_t *)(ring_slots + idx * ring->slot_size);
}

/// Internal helper function to insert an element into the next empty slot.
/// Check for space should be done by the caller.
static __attribute__((always_inline)) inline void __jring2_insert(
    jring2_t *ring, const void *obj) {
  jring2_entry_t *slot = __jring2_get_slot(ring, ring->write_idx);

  // Memory copy the element.
  memcpy(slot->data, obj, ring->element_size);
  asm volatile("" ::: "memory");
  slot->dd = 1;
  ring->write_idx = (ring->write_idx + 1) & ring->mask;
}

/**
 * Calculate the memory size needed for a ring with given element number.
 *
 * This function returns the number of bytes needed for a ring, given
 * the number of elements in it.
 * This is the sum of the size of the structure jring2_t and the size of the
 * memory needed for storing the elements. The value is aligned to a cache
 * line size.
 *
 * @param element_size
 *   The size of each ring element, in bytes. It must be a multiple of 4B.
 *   *Attention* This is different than the ring slot size, which includes
 *   `sizeof(jring2_entry_t)` header.
 * @param count
 *   The number of elements in the ring (must be a power of 2).
 * @return
 *   - The memory size needed for the ring on success.
 *   - (size_t)-1 - Element count is not a power of 2.
 */
static inline size_t jring2_get_buf_ring_size(uint32_t element_size,
                                              uint32_t count) {
  if ((element_size % 4 != 0)) return -1;
  if (count == 0 || !ISPOWEROF2(count)) {
    return -1;
  }

  uint32_t slot_size = sizeof(jring2_entry_t) + element_size;
  size_t sz = sizeof(jring2_t) + count * slot_size;
  sz = ALIGN_UP_POW2(sz, CACHELINE_SIZE);
  return sz;
}

/**
 * Function to initialize a ring.
 *
 * @param r Pointer to the ring structure.
 * @param n_ent The number of elements in the ring (must be a power of 2).
 * @return 0 on success, -EINVAL on failure.
 */
static inline int jring2_init(jring2_t *r, uint32_t n_ent, uint32_t esize) {
  if ((esize % 4 != 0)) return -EINVAL;
  if (n_ent == 0 || !ISPOWEROF2(n_ent)) {
    return -EINVAL;
  }

  r->cnt = n_ent;
  r->mask = r->cnt - 1;
  // The element size is the size of the object plus the size of the entry
  // metadata.
  r->element_size = esize;
  r->slot_size = r->element_size + sizeof(jring2_entry_t);
  r->write_idx = 0;
  r->read_idx = 0;
  r->free_write_cnt = r->mask;

  // Iterate over the ring and initialize the slot metadata.
  for (uint32_t i = 0; i < r->cnt; i++) {
    jring2_entry_t *slot = __jring2_get_slot(r, i);
    slot->dd = 0;
  }
  return 0;
}

/**
 * @brief Returns the number of elements enqueued in the ring. This could be a
 * conservative estimate (i.e., it might be stale).
 */
static inline __attribute__((always_inline)) uint32_t jring2_count(
    jring2_t *ring) {
  ring->free_write_cnt =
      (ring->read_idx - ring->write_idx + ring->cnt - 1) & ring->mask;
  asm volatile("" ::: "memory");
  return ring->mask - ring->free_write_cnt;
}

/**
 * Enqueue one object on a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj
 *   A pointer to the object to be enqueued.
 * @return
 *   1 if the object is successfully enqueued, 0 otherwise.
 */
static inline __attribute__((always_inline)) uint32_t jring2_enqueue(
    jring2_t *ring, const void *obj) {
  if (ring->free_write_cnt == 0) {
    const uint32_t rd_idx = ring->read_idx;
    asm volatile("" ::: "memory");

    // We need to calculate number of slots from writer to reader, which
    // requires some circular arithmetic.
    ring->free_write_cnt =
        (rd_idx - ring->write_idx + ring->cnt - 1) & ring->mask;
    if (ring->free_write_cnt == 0) {
      // In single mode, we either enqueue all or none.
      return 0;
    }
  }

  __jring2_insert(ring, obj);
  ring->free_write_cnt--;
  return 1;
}

/**
 * Enqueue a specific amount of objects on a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of objects.
 * @return
 *   The number of objects enqueued, either 0 or n
 */
static inline __attribute__((always_inline)) uint32_t jring2_enqueue_bulk(
    jring2_t *ring, const void *obj_table, uint32_t n) {
  if (ring->free_write_cnt < n) {
    const uint32_t rd_idx = ring->read_idx;
    asm volatile("" ::: "memory");

    // We need to calculate number of slots from writer to reader, which
    // requires some circular arithmetic.
    ring->free_write_cnt =
        (rd_idx - ring->write_idx + ring->cnt - 1) & ring->mask;
    if (ring->free_write_cnt < n) {
      // In bulk mode, we either enqueue all or none.
      return 0;
    }
  }

  uint32_t index = 0;
  do {
    const uint8_t *src_obj = (uint8_t *)obj_table + index * ring->element_size;
    __jring2_insert(ring, src_obj);
  } while (++index < n);
  ring->free_write_cnt -= n;

  return n;
}

static __attribute__((always_inline)) inline uint32_t jring2_dequeue(
    jring2_t *ring, void *elem) {
  jring2_entry_t *slot = __jring2_get_slot(ring, ring->read_idx);
  if (slot->dd == 0) {
    return 0;
  }

  // Memory copy the element.
  memcpy(elem, slot->data, ring->element_size);
  asm volatile("" ::: "memory");
  slot->dd = 0;  // Mark the slot as empty.
  ring->read_idx = (ring->read_idx + 1) & ring->mask;
  return 1;
}

static __attribute__((always_inline)) inline uint32_t jring2_dequeue_burst(
    jring2_t *ring, void *obj_table, uint32_t n) {
  uint32_t cnt = 0;
  while (cnt < n) {
    uint8_t *dst_obj = (uint8_t *)obj_table + cnt * ring->element_size;
    uint32_t ret = jring2_dequeue(ring, dst_obj);
    if (ret != 1) {
      break;
    }
    cnt++;
  }
  return cnt;
}

#ifdef __cplusplus
}
#endif

#endif  // SRC_EXT_JRING2_H_
