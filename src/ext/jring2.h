/*
MIT License

Copyright (c) 2018 Meng Rao <raomeng1@gmail.com>
Copyright (c) 2013 Anuj Kalia<ankalia@microsoft.comom>

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

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

#define CACHELINE_SIZE 64

typedef struct {
  uint64_t full;  // Updated by both writing and reading thread
  char data[CACHELINE_SIZE - sizeof(uint64_t)];
} jring2_ent_t __attribute__((aligned(CACHELINE_SIZE)));

/// @brief SPSC ring buffer with cache line sized entries
typedef struct {
  uint32_t cnt;
  uint8_t pad0[CACHELINE_SIZE];
  jring2_ent_t* blk;

  uint8_t pad1[CACHELINE_SIZE];
  uint64_t write_idx;  // Used only by writing thread
  // Number of slots the writer can write to without overwriting unread entries
  uint64_t free_write_cnt;

  uint8_t pad3[CACHELINE_SIZE];
  volatile uint64_t read_idx;  // Used by both writing and reading thread
} jring2_t;

// n_ent must be a power of 2
static inline int jring2_init(jring2_t* cl_ring, uint32_t n_ent) {
  if (n_ent == 0 || (n_ent & (n_ent - 1)) != 0) {
    return -EINVAL;
  }

  cl_ring->blk =
      (jring2_ent_t*)memalign(CACHELINE_SIZE, n_ent * sizeof(jring2_ent_t));
  cl_ring->write_idx = 0;
  cl_ring->read_idx = 0;
  cl_ring->free_write_cnt = n_ent - 1;

  cl_ring->cnt = n_ent;
  return 0;
}

static inline void jring2_deinit(jring2_t* ring) {free(ring->blk); }

static int jring2_enqueue(jring2_t* ring, const jring2_ent_t* ent) {
  if (ring->free_write_cnt == 0) {
    const uint32_t rd_idx = ring->read_idx;
    asm volatile("" ::: "memory");

    // We need to calculate number of slots from writer to reader, which
    // requires some cirular arithmetic.
    ring->free_write_cnt =
        (rd_idx - ring->write_idx + ring->cnt - 1) & (ring->cnt - 1);
    if (ring->free_write_cnt == 0) {
      return -ENOBUFS;
    }
  }

  ring->blk[ring->write_idx] = *ent;
  asm volatile("" ::: "memory");
  ring->blk[ring->write_idx].full = 1;
  ring->write_idx = (ring->write_idx + 1) & (ring->cnt - 1);

  return 0;
}

static int jring2_dequeue(jring2_t* ring, jring2_ent_t* ent) {
  if (ring->blk[ring->read_idx].full == 0) {
    return -ENOENT;
  }

  *ent = ring->blk[ring->read_idx];
  asm volatile("" ::: "memory");
  ring->blk[ring->read_idx].full = 0;
  ring->read_idx = (ring->read_idx + 1) & (ring->cnt - 1);

  return 0;
}

#endif /* JRING2_H */
