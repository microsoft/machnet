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

#ifndef JRING2_H
#define JRING2_H

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
} jring2_entry_t __attribute__((aligned(CACHELINE_SIZE)));

/// @brief SPSC ring buffer with cache line sized entries
typedef struct {
  uint32_t cnt;
  uint32_t mask;
  uint32_t element_size;

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
 *   The size of ring element, in bytes. It must be a multiple of 4B.
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
  r->element_size = esize + sizeof(jring2_entry_t);
  r->write_idx = 0;
  r->read_idx = 0;
  r->free_write_cnt = r->cnt - 1;

  // Iterate over the ring and initialize the slot metadata.
  uint8_t *ring_slots = (uint8_t *)(r + 1);
  for (uint32_t i = 0; i < r->cnt; i++) {
    jring2_entry_t *slot =
        (jring2_entry_t *)(ring_slots + i * r->element_size);
    slot->dd = 0;
  }
  return 0;
}

static __attribute((always_inline)) inline int jring2_enqueue(
    jring2_t *ring, const void *elem) {
  if (ring->free_write_cnt == 0) {
    const uint32_t rd_idx = ring->read_idx;
    asm volatile("" ::: "memory");

    // We need to calculate number of slots from writer to reader, which
    // requires some circular arithmetic.
    ring->free_write_cnt =
        (rd_idx - ring->write_idx + ring->cnt - 1) & ring->mask;
    if (ring->free_write_cnt == 0) {
      return -ENOBUFS;
    }
  }

  uint8_t *ring_slots = (uint8_t *)(ring + 1);
  jring2_entry_t *slot =
      (jring2_entry_t *)(ring_slots + ring->write_idx * ring->element_size);

  // Memory copy the element.
  memcpy(slot->data, elem, ring->element_size - sizeof(jring2_entry_t));
  asm volatile("" ::: "memory");
  slot->dd = 1;
  ring->write_idx = (ring->write_idx + 1) & ring->mask;

  return 0;
}

static __attribute((always_inline)) inline int jring2_dequeue(jring2_t *ring,
                                                       void *elem) {
  uint8_t *ring_slots = (uint8_t *)(ring + 1);
  jring2_entry_t *slot =
      (jring2_entry_t *)(ring_slots + ring->read_idx * ring->element_size);
  if (slot->dd == 0) {
    return -ENOENT;
  }

  // Memory copy the element.
  memcpy(elem, slot->data, ring->element_size - sizeof(jring2_entry_t));
  asm volatile("" ::: "memory");
  slot->dd = 0; // Mark the slot as empty.
  ring->read_idx = (ring->read_idx + 1) & ring->mask;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* JRING2_H */
