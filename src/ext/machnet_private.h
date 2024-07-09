/**
 * @file machnet_private.h
 *
 * Helper functions to integreate Machnet with shared memory channels.
 * This is an internal API, not to be used from applications.
 */
#ifndef SRC_EXT_MACHNET_PRIVATE_H_
#define SRC_EXT_MACHNET_PRIVATE_H_
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
using namespace boost::interprocess;

// #ifdef __cplusplus
// extern "C" {
// #endif

#ifdef __linux__
#include <sys/mman.h>
#include <sys/types.h>
#else // #ifdef __linux__


#endif // #ifdef __linux__

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include "machnet_common.h"

// Debugging
#include <iostream>
#include <typeinfo>
#include <windows.h>

#define MACHNET_CHANNEL_CTRL_SQ_SLOT_NR 2
#define MACHNET_CHANNEL_CTRL_CQ_SLOT_NR (MACHNET_CHANNEL_CTRL_SQ_SLOT_NR)

// Count the number of array elements.
// clang-format off
#define COUNT_OF(x) \
  ((sizeof(x) / sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
// clang-format on

#define IS_POW2(x) (((x) & ((x)-1)) == 0)
// Macro to round up to next power of 2.
#define ROUNDUP_U64_POW2(x) (1ULL << (64 - __builtin_clzll(((uint64_t)x) - 1)))

static inline size_t get_win_pagesize() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}

/**
 * Calculate the memory size needed for an Machnet Dataplane channel.
 *
 * An Machnet Dataplane channel contains two rings for message passing in each
 * direction (Machnet -> Application, Application -> NSaas), and one ring that
 * holds free buffers (used for allocations).
 *
 * This function returns the number of bytes needed for the channel area, given
 * the number of elements in each of the rings of the channel and the desired
 * buffer size.
 *
 * @param machnet_ring_slot_nr The number of Machnet->App messaging ring slots
 * (must be power of 2).
 * @param app_ring_slot_nr   The number of App->Machnet messaging ring slots
 * (must be power of 2).
 * @param buf_ring_slot_nr   The number of buffers + 1 in the pool (must be
 *                           power of 2).
 * @param buffer_size        The usable size of each buffer.
 * @param is_posix_shm       Whether the channel will be a POSIX shared memory.
 * @return
 *   - The memory size in bytes needed for the Machnet channel on success.
 *   - (size_t)-1 - Some parameter is not a power of 2, or the buffer size is
 *                  bad (too big).
 */
static inline size_t __machnet_channel_dataplane_calculate_size(
    size_t machnet_ring_slot_nr, size_t app_ring_slot_nr,
    size_t buf_ring_slot_nr, size_t buffer_size, int is_posix_shm) {
  
  std::cout << "inside __machnet_channel_dataplane_calculate_size" << std::endl;
  
  std::cout << "before checking if params are power of 2" << std::endl;
  // Check that all parameters are power of 2.
  if (!IS_POW2(machnet_ring_slot_nr) || !IS_POW2(app_ring_slot_nr) ||
      !IS_POW2(buf_ring_slot_nr))
    return -1;
  std::cout << "after checking if params are power of 2" << std::endl;

  const size_t total_buffer_size =
      ROUNDUP_U64_POW2(buffer_size + MACHNET_MSGBUF_SPACE_RESERVED +
                       MACHNET_MSGBUF_HEADROOM_MAX);

  std::cout << "total_buffer_size: " << total_buffer_size << std::endl;
  std::cout << "Given buffer_size: " << buffer_size << std::endl;

  // const size_t kPageSize = (is_posix_shm ? getpagesize() : HUGE_PAGE_2M_SIZE);
  // if (buffer_size > kPageSize) return -1;

  // Add the size of the channel's header.
  size_t total_size = sizeof(MachnetChannelCtx_t);

  // Size of statistics structure.
  total_size += sizeof(MachnetChannelStats_t);

  // Add the size of the control rings.
  size_t ctrl_ring_sizes[] = {MACHNET_CHANNEL_CTRL_SQ_SLOT_NR,
                              MACHNET_CHANNEL_CTRL_CQ_SLOT_NR};
  for (size_t i = 0; i < COUNT_OF(ctrl_ring_sizes); i++) {
    size_t acc = jring_get_buf_ring_size(sizeof(MachnetCtrlQueueEntry_t),
                                         ctrl_ring_sizes[i]);
    if (acc == (size_t)-1) return -1;
    total_size += acc;
  }

  // Add the size of the rings (Machnet, Application, BufferRing).
  size_t data_ring_sizes[] = {machnet_ring_slot_nr, app_ring_slot_nr,
                              buf_ring_slot_nr};
  for (size_t i = 0; i < COUNT_OF(data_ring_sizes); i++) {
    size_t acc =
        jring_get_buf_ring_size(sizeof(MachnetRingSlot_t), data_ring_sizes[i]);
    if (acc == (size_t)-1) return -1;
    total_size += acc;
  }

  // Align to cache line boundary, and add the size of the scratch buffer index
  // table.
  total_size = ALIGN_TO_BOUNDARY(total_size, CACHE_LINE_SIZE);
  total_size += buf_ring_slot_nr * sizeof(MachnetRingSlot_t);

  // Align to page boundary.
  // total_size = ALIGN_TO_BOUNDARY(total_size, kPageSize);

  // Add the size of the buffers.
  total_size += buf_ring_slot_nr * total_buffer_size;

  // Align to page boundary.
  // total_size = ALIGN_TO_BOUNDARY(total_size, kPageSize);

  std::cout << "before returning total_size: " << total_size << std::endl;
  return total_size;
}

/**
 * Initialiaze an Machnet Dataplane channel.
 *
 * This function initializes the memory of an Machnet Dataplane channel. It
 * initializes the context, the rings and buffers required to facilitate
 * bidirectional communication.
 *
 * @param shm                Pointer to the shared memory area.
 * @param shm_size           Size of the shared memory area.
 * @param is_posix_shm       Whether the channel is based on POSIX shared
 *                           memory(1, or 0 otherwise).
 * @param name               The name of the channel.
 * @param machnet_ring_slot_nr The number of Machnet->App messaging ring slots.
 * @param app_ring_slot_nr   The number of App->Machnet messaging ring slots.
 * @param buf_ring_slot_nr   The number of buffers + 1 to be used in this
 *                           channel (must sum up to a power of 2).
 * @param buffer_size        The size of each buffer.
 * @param is_multithread     1 if Machnet is using multiple threads per channel,
 * 0 otherwise.
 * @return                   '0' on success, '-1' on failure.
 */
static inline int __machnet_channel_dataplane_init(
    uchar_t *shm, size_t shm_size, int is_posix_shm, const char *name,
    size_t machnet_ring_slot_nr, size_t app_ring_slot_nr,
    size_t buf_ring_slot_nr, size_t buffer_size, int is_multithread,
    shared_memory_object &shm_obj, mapped_region &region) {

  std::cout << "inside __machnet_channel_dataplane_init" << std::endl;
  std::cout << "type of shm pointer: " << typeid(shm).name() << std::endl;

  size_t total_size = __machnet_channel_dataplane_calculate_size(
      machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      is_posix_shm);

  std::cout << "new calc size: " << total_size << std::endl;
  std::cout << "given size: " << shm_size << std::endl;

  // Guard against mismatches.
  if (total_size > shm_size || total_size == (size_t)-1) return -1;

  // TODO(ilias): Check that we can always accomodate an MACHNET_MSG_MAX_LEN
  // sized mesage with the number of buffers and buffer_size provided here.

  std::cout << "before creating memory layout __machnet_channel_dataplane_init" << std::endl;

  // Check the memory layout in "machnet_common.h".
  // Initialize the channel context.

  if(shm == NULL) std::cout << "uchar_t* shm is NULL" << std::endl;
  else std::cout << "uchar_t* shm is okay" << std::endl;

  MachnetChannelCtx_t *ctx = reinterpret_cast<MachnetChannelCtx_t *> (shm);
  // MachnetChannelCtx_t *ctx = shm;
  
  std::cout << "type of ctx pointer: " << typeid(ctx).name() << std::endl;
  if(ctx == NULL) std::cout << "MachnetChannelCtx_t *ctx is NULL" << std::endl;
  else std::cout << "MachnetChannelCtx_t *ctx is okay" << std::endl;
  std::cout << "shm pointer is copied into MachnetChannelCtx_t *" << std::endl;
  std::cout << "size of MachnetChannelCtx_t *ctx" << sizeof(*ctx) << std::endl;


  std::cout << "inside dataplane_init, region size: " << region.get_size() << std::endl;
  std::cout << "before setting ctx->version: " << MACHNET_CHANNEL_VERSION << std::endl;

  ctx->version = MACHNET_CHANNEL_VERSION;
  std::cout << "ctx->version is set: " << MACHNET_CHANNEL_VERSION << std::endl;

  ctx->size = total_size;
  std::cout << "ctx->size is set: " << total_size << std::endl;

  strncpy(ctx->name, name, sizeof(ctx->name));
  std::cout << "ctx->name got the name copied: " << name << std::endl;

  ctx->name[sizeof(ctx->name) - 1] = '\0';
  std::cout << "ctx->name is appended with 0: " << name << std::endl;

  std::cout << "initialized channel context" << std::endl;

  // Initiliaze the ctrl context.
  ctx->ctrl_ctx.req_id = 0;

  // Initialize buffer cache
  ctx->app_buffer_cache.count = 0;

  std::cout << "Before __machnet_channel_mem_ofs" << std::endl;

  // Clear out statatistics.
  ctx->data_ctx.stats_ofs = sizeof(*ctx);
  MachnetChannelStats_t *stats =
      (MachnetChannelStats_t *)__machnet_channel_mem_ofs(
          ctx, ctx->data_ctx.stats_ofs);
  memset(stats, 0, sizeof(*stats));
  
  std::cout << "After __machnet_channel_mem_ofs" << std::endl;

  const int kMultiThread = 1;  // Assume the application always multithreaded.

  std::cout << "before jring_init" << std::endl;

  // Ring0 (ctrl - SQ) follows immediately after the statistics.
  ctx->data_ctx.ctrl_sq_ring_ofs = ctx->data_ctx.stats_ofs + sizeof(*stats);
  int ret = jring_init(__machnet_channel_ctrl_sq_ring(ctx),
                       MACHNET_CHANNEL_CTRL_SQ_SLOT_NR,
                       sizeof(MachnetCtrlQueueEntry_t), is_multithread, 0);

  std::cout << "after Ring0 ret: " << ret << std::endl;

  if (ret != 0) return ret;

  // Ring1 (ctrl - CQ) follows immediately after the first ring.
  ctx->data_ctx.ctrl_cq_ring_ofs =
      ctx->data_ctx.ctrl_sq_ring_ofs +
      jring_get_buf_ring_size(sizeof(MachnetCtrlQueueEntry_t),
                              MACHNET_CHANNEL_CTRL_SQ_SLOT_NR);
  ret = jring_init(__machnet_channel_ctrl_cq_ring(ctx),
                   MACHNET_CHANNEL_CTRL_CQ_SLOT_NR,
                   sizeof(MachnetCtrlQueueEntry_t), 0, is_multithread);

  std::cout << "after Ring1 ret: " << ret << std::endl;

  if (ret != 0) return ret;

  // Initialize the Machnet->Application ring.
  ctx->data_ctx.machnet_ring_ofs =
      ctx->data_ctx.ctrl_cq_ring_ofs +
      jring_get_buf_ring_size(sizeof(MachnetCtrlQueueEntry_t),
                              MACHNET_CHANNEL_CTRL_CQ_SLOT_NR);

  jring_t *machnet_ring = __machnet_channel_machnet_ring(ctx);
  ret = jring_init(machnet_ring, machnet_ring_slot_nr,
                   sizeof(MachnetRingSlot_t), is_multithread, kMultiThread);

  std::cout << "after Machnet -> Application ring, ret: " << ret << std::endl;

  if (ret != 0) return ret;

  // App->Machnet ring follows immediately after the Machnet->App ring.
  ctx->data_ctx.app_ring_ofs =
      ctx->data_ctx.machnet_ring_ofs +
      jring_get_buf_ring_size(sizeof(MachnetRingSlot_t), machnet_ring_slot_nr);
  jring_t *app_ring = __machnet_channel_app_ring(ctx);
  ret = jring_init(app_ring, app_ring_slot_nr, sizeof(MachnetRingSlot_t),
                   kMultiThread, is_multithread);

  std::cout << "after App -> Machnet ring, ret: " << ret << std::endl;
                  
  if (ret != 0) return ret;

  // jring_get_buf_ring_size() cannot fail here.
  ctx->data_ctx.buf_ring_ofs =
      ctx->data_ctx.app_ring_ofs +
      jring_get_buf_ring_size(sizeof(MachnetRingSlot_t), app_ring_slot_nr);

  // Initialize the buffer ring.
  jring_t *buf_ring = __machnet_channel_buf_ring(ctx);
  ret = jring_init(buf_ring, buf_ring_slot_nr, sizeof(MachnetRingSlot_t),
                   kMultiThread, kMultiThread);

  std::cout << "after initializing the buffer ring, ret: " << ret << std::endl;

  if (ret != 0) return ret;

  // Offset in memory channel where the final ring ends (buf_ring).
  size_t buf_ring_end_ofs =
      ctx->data_ctx.buf_ring_ofs +
      jring_get_buf_ring_size(sizeof(MachnetRingSlot_t), buf_ring_slot_nr);

  // Offset in memory, of the scratch buffer index table.
  ctx->data_ctx.buffer_index_table_ofs =
      ALIGN_TO_BOUNDARY(buf_ring_end_ofs, CACHE_LINE_SIZE);
  size_t tmp_buffer_index_table_end_ofs =
      ctx->data_ctx.buffer_index_table_ofs +
      buf_ring_slot_nr * sizeof(MachnetRingSlot_t);

  // Calculate the actual buffer size (incl. metadata).
  const size_t kTotalBufSize =
      ROUNDUP_U64_POW2(buffer_size + MACHNET_MSGBUF_SPACE_RESERVED +
                       MACHNET_MSGBUF_HEADROOM_MAX);

  // Initialize the buffers. Note that the buffer pool start is aligned to the
  // page_size boundary.
  // const size_t kPageSize = is_posix_shm ? getpagesize() : HUGE_PAGE_2M_SIZE;
  // ctx->data_ctx.buf_pool_ofs =
  //     ALIGN_TO_BOUNDARY(tmp_buffer_index_table_end_ofs, kPageSize);

  const size_t kPageSize = get_win_pagesize(); // hardcoding 4KB - need to confirm
  ctx->data_ctx.buf_pool_ofs =
      ALIGN_TO_BOUNDARY(tmp_buffer_index_table_end_ofs, kPageSize);

  ctx->data_ctx.buf_pool_mask = buf_ring->capacity;
  ctx->data_ctx.buf_size = kTotalBufSize;
  ctx->data_ctx.buf_mss = buffer_size;

  std::cout << "before initializing the message header of each buffer: " << std::endl;

  // Initialize the message header of each buffer.
  for (uint32_t i = 0; i < buf_ring->capacity; i++) {
    MachnetMsgBuf_t *buf = __machnet_channel_buf(ctx, i);
    __machnet_channel_buf_init(buf);
    // The following fields should only be initialized once here.
    *__DECONST(uint32_t *, &buf->magic) = MACHNET_MSGBUF_MAGIC;
    *__DECONST(uint32_t *, &buf->index) = i;
    *__DECONST(uint32_t *, &buf->size) =
        buffer_size + MACHNET_MSGBUF_HEADROOM_MAX;
  }

  std::cout << "before initializing the message header of each buffer: " << std::endl;

  // Initialize the buffer index table, and make all these buffers available.
  MachnetRingSlot_t *buf_index_table = (MachnetRingSlot_t *)malloc(
      buf_ring->capacity * sizeof(MachnetRingSlot_t));

  std::cout << "After initializing buffer index table" << std::endl;

  if (buf_index_table == NULL) {
    std::cout << "buf_index_table is NULL, returning" << std::endl;
    return -1;
  }

  for (size_t i = 0; i < buf_ring->capacity; i++) buf_index_table[i] = i;

  unsigned int free_space;
  int enqueued = jring_enqueue_bulk(buf_ring, buf_index_table,
                                    buf_ring->capacity, &free_space);
  free(buf_index_table);

  if (((size_t)enqueued != buf_ring->capacity) || (free_space != 0)) {
    std::cout << "Enqueue has failed, returning" << std::endl;
    return -1;  // Enqueue has failed.
  }
    

  // Set the header magic at the end.
  __sync_synchronize();
  ctx->magic = MACHNET_CHANNEL_CTX_MAGIC;
  __sync_synchronize();

  return 0;
}

/**
 * This function creates a POSIX shared memory region to be used as an Machnet
 * channel. The shared memory region is created with the given name and size and
 * does not support huge pages.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in] channel_size           Size of the usable memory area of the
 * channel in bytes.
 * @param[out] shm_fd             Sets the file descriptor accordingly (-1 on
 *                           failure, >0 on success).
 * @return                   Pointer to channel's memory area on success, NULL
 *                           otherwise.
 */
static inline MachnetChannelCtx_t *__machnet_channel_posix_create(
    const char *channel_name, size_t channel_size, int *shm_fd) {
  std::cout << "__machnet_channel_posix_create fired" << std::endl;
  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  MachnetChannelCtx_t *channel = NULL;
  int shm_flags, prot_flags;

  // // Create the shared memory segment.
  // *shm_fd = shm_open(channel_name, O_CREAT | O_EXCL | O_RDWR, 0666);
  // if (*shm_fd < 0) {
  //   perror("shm_open()");
  //   return NULL;
  // }

  // // Set the size of the shared memory segment.
  // if (ftruncate(*shm_fd, channel_size) == -1) {
  //   perror("ftruncate()");
  //   goto fail;
  // }

  // Map the shared memory segment into the address space of the process.
  // prot_flags = PROT_READ | PROT_WRITE;
  // shm_flags = MAP_SHARED | MAP_POPULATE;
  // channel = (MachnetChannelCtx_t *)mmap(NULL, channel_size, prot_flags,
  //                                       shm_flags, *shm_fd, 0);
  // if (channel == MAP_FAILED) {
  //   perror("mmap()");
  //   goto fail;
  // }

  // Lock the memory segment in RAM.
  // if (mlock((void *)channel, channel_size) != 0) {
  //   perror("mlock()");
  //   goto fail;
  // }

  return channel;

fail:
  // if (channel != NULL && channel != MAP_FAILED) munmap(channel, channel_size);

  // if (*shm_fd != -1) {
  //   close(*shm_fd);
  //   shm_unlink(channel_name);
  //   *shm_fd = -1;
  // }
  return NULL;
}

/**
 * This function creates a POSIX shared memory region to be used as an Machnet
 * channel. The shared memory region is created with the given name and size and
 * does not support huge pages.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in]  channel_size          Size of the usable memory area of the
 * channel (in bytes, huge pages aligned)
 * @param[out] shm_fd                Sets the file descriptor accordingly (-1 on
 * failure, >0 on success).
 * @return                   Pointer to channel's memory area on success, NULL
 *                           otherwise.
 */
static inline MachnetChannelCtx_t *__machnet_channel_hugetlbfs_create(
    const char *channel_name, size_t channel_size, int *shm_fd, 
    shared_memory_object &shm_obj, mapped_region &region) {

  std::cout << "__machnet_channel_hugetlbfs_create fired" << std::endl;
  std::cout << "checking channel_name and shm_fd assertions" << std::endl;

  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  MachnetChannelCtx_t *channel = NULL;

  #ifdef __linux__
    int shm_flags;

    // Check if channel size is huge page aligned.
    if ((channel_size & (HUGE_PAGE_2M_SIZE - 1)) != 0) {
      fprintf(stderr, "Channel size %zu is not huge page aligned.\n",
              channel_size);
      return NULL;
    }

    // Create the shared memory segment.
    *shm_fd = memfd_create(channel_name, MFD_HUGETLB);
    if (*shm_fd < 0) {
      fprintf(stderr, "memfd_create() failed, error = %s\n", strerror(errno));
      return NULL;
    }

    // Set the size of the shared memory segment.
    if (ftruncate(*shm_fd, channel_size) == -1) {
      fprintf(stderr,
              "%s: ftruncate() failed, error = %s. This can happen if (1) there "
              "are no hugepages, or (2) the hugepage size is not 2MB.\n",
              __FILE__, strerror(errno));
      goto fail;
    }

    // Map the shared memory segment into the address space of the process.
    shm_flags = MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
    channel = (MachnetChannelCtx_t *)mmap(
        NULL, channel_size, PROT_READ | PROT_WRITE, shm_flags, *shm_fd, 0);
    if (channel == MAP_FAILED) {
      fprintf(stderr, "mmap() failed, error = %s\n", strerror(errno));
      goto fail;
    }

    // Lock the memory segment in RAM.
    if (mlock((void *)channel, channel_size) != 0) {
      fprintf(stderr, "mlock() failed, error = %s\n", strerror(errno));
      goto fail;
    }

    return channel;

  fail:
    if (channel != NULL && channel != MAP_FAILED) munmap(channel, channel_size);

    if (*shm_fd != -1) {
      close(*shm_fd);
    }
    *shm_fd = -1;
    return NULL;
  #else // #ifdef __linux__

    std::cout << "inside windows block in __machnet_channel_hugetlbfs_create" << std::endl;
    // Check if channel size is huge page aligned. [RR: skipping for now]

    std::cout << "before creating the shared_memory_objet shm_obj" << std::endl;

    // Create the shared memory segment.
    // shared_memory_object shm_obj(
    //   create_only,
    //   channel_name,
    //   read_write
    // );

    shm_obj = shared_memory_object (
      create_only,
      channel_name,
      read_write
    );

    std::cout << "after creating the shared_memory_objet shm_obj" << std::endl;
    std::cout << "before setting size of the shared_memory_objet shm_obj" << std::endl;

    // Set the size of the shared memory segment.
    shm_obj.truncate(static_cast<offset_t>(channel_size));    
    
    std::cout << "before mapping shm segment into address space of process" << std::endl;

    // Map the shared memory segment into the address space of the process.
    // mapped_region region(shm_obj, read_write);
    
    region = mapped_region(
      shm_obj,
      read_write
    );

    channel = reinterpret_cast<MachnetChannelCtx_t *> (region.get_address());

    std::cout << "shared memory segment channel type: " << typeid(channel).name() << std::endl;

    std::cout << "After getting the mapped_region address into MachnetChannelCtx_t *channel" << std::endl;
    if(channel == NULL) std::cout << "channel is NULL from mapped_region get_address()" << std::endl;
    else std::cout << "channel is not NULL from mapped_region get_address()" << std::endl;

    // Lock the memory segment in RAM. [RR: skipping for now]

    // test
    std::cout << "before test init of channel->version" << std::endl;
    channel->version = 1;
    std::cout << "after test init of channel->version" << std::endl;
    std::cout << "channel version: " << channel->version << std::endl;

    std::cout << "mapped region size: " << region.get_size() << std::endl;

    // setting a placeholder fd [RR: update to have no fd]
    *shm_fd = 99;
    return channel;
  #endif // #ifdef __linux__
  return NULL;
}

/**
 * This function unmaps, and destroys an Machnet channel, releasing the shared
 * memory segment.
 *
 * @param[in] mapped_mem             Pointer to the mapped memory area of the
 * channel.
 * @param[in] mapped_mem_size        Size of the mapped memory area.
 * @param[in] shm_fd                 Opened file descriptor for the shared
 * memory segment.
 * @param[in] is_posix_shm           0 if this is not POSIX shmem segment.
 * @param[in] channel_name           The name of the shared memory segment (if
 * POSIX shmem). Can be NULL if this is not POSIX shmem.
 */
static inline void __machnet_channel_destroy(void *mapped_mem,
                                             size_t mapped_mem_size,
                                             int *shm_fd, int is_posix_shm,
                                             const char *channel_name) {
  assert(mapped_mem != NULL);
  assert(mapped_mem_size > 0);

  // Unmap the shared memory segment.
  // munmap(mapped_mem, mapped_mem_size);
  // if (shm_fd != NULL && *shm_fd >= 0) {
  //   close(*shm_fd);
  //   *shm_fd = -1;
  // }

  if (is_posix_shm) {
    assert(channel_name != NULL);
    // shm_unlink(channel_name);
  }
}

/**
 * This function creates a shared memory region to be used as an Machnet
 * channel.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in] machnet_ring_slot_nr     Number of slots in the Machnet ring.
 * @param[in] app_ring_slot_nr       Number of slots in the application ring.
 * @param[in] buf_ring_slot_nr       Number of slots in the buffer ring.
 * @param[out] channel_mem_size      (ptr) The real size of the underlying
 * shared memory segment. Can differ from `channel_size` because of alignment
 * reasons (e.g, 4K or 2MB).
 * @param[out] is_posix_shm          (ptr) Set to 1 if this is a POSIX shared
 * memory segment (not backed by hugetlbfs)
 * @param[out] shm_fd                Sets the file descriptor accordingly (-1 on
 *                                   failure, >0 on success).
 * @return                           Pointer to channel's memory area on
 * success, NULL otherwise.
 */
static inline MachnetChannelCtx_t *__machnet_channel_create(
    const char *channel_name, size_t machnet_ring_slot_nr,
    size_t app_ring_slot_nr, size_t buf_ring_slot_nr, size_t buffer_size,
    size_t *channel_mem_size, int *is_posix_shm, int *shm_fd, 
    shared_memory_object &shm_obj, mapped_region &region) {

  std::cout << "inside __machnet_channel_create, checking assertions" << std::endl;
  if(channel_name == NULL) std::cout << "channel_name is NULL" << std::endl;
  if(shm_fd == NULL) std::cout << "shm_fd is NULL" << std::endl;
  if(channel_mem_size == NULL) std::cout << "channel_mem_size is NULL" << std::endl;
  if(is_posix_shm == NULL) std::cout << "is_posix_shm is NULL" << std::endl;

  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  assert(channel_mem_size != NULL);
  assert(is_posix_shm != NULL);
  assert(shm_fd != NULL);

  std::cout << "after asstertions, nothing is NULL" << std::endl;

  MachnetChannelCtx_t *channel;

  std::cout << "Before calling __machnet_channel_dataplane_calculate_size" << std::endl;

  *is_posix_shm = 0;
  *channel_mem_size = __machnet_channel_dataplane_calculate_size(
      machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      *is_posix_shm);

  std::cout << "After __machnet_channel_dataplane_calculate_size, channel_mem_size: " << channel_mem_size << std::endl;
  
  std::cout << "Before calling __machnet_channel_hugetlbfs_create" << std::endl;
  // Try creating and mapping a hugetlbfs backed shared memory segment.
  channel = __machnet_channel_hugetlbfs_create(channel_name, *channel_mem_size,
                                               shm_fd, shm_obj, region);

  std::cout << "After __machnet_channel_hugetlbfs_create, size: " << region.get_size() << std::endl;                                             
  if (channel != NULL) {
    std::cout << "MachnetChannelCtx_t *channel is not NULL, going to out" << std::endl;
    goto out;
  }

  fprintf(stderr,
          "Failed to create hugetlbfs backed shared memory segment; falling "
          "back to POSIX shm.\n");

  // Hugetlbfs backed shared memory segment creation failed. Fallback to a
  // regular POSIX shm segment.
  *is_posix_shm = 1;
  *channel_mem_size = __machnet_channel_dataplane_calculate_size(
      machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      *is_posix_shm);
  channel =
      __machnet_channel_posix_create(channel_name, *channel_mem_size, shm_fd);
  if (channel != NULL) goto out;

  // Failed to create shared memory segment.
  return NULL;

out:
  std::cout << "inside out block in __machnet_channel_create" << std::endl;
  std::cout << "before __machnet_channel_dataplane_init" << std::endl;

  // The shared memory segment is created and mapped. Initialize it.
  // int ret = __machnet_channel_dataplane_init(
  //     (uchar_t *)channel, *channel_mem_size, *is_posix_shm, channel_name,
  //     machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size, 0);
  
  int ret = __machnet_channel_dataplane_init(
    (uchar_t*)channel, *channel_mem_size, *is_posix_shm, channel_name,
    machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size, 0,
    shm_obj, region);

  std::cout << "after __machnet_channel_dataplane_init, ret: " << ret << std::endl;
  
  if (ret != 0) {
    __machnet_channel_destroy((void *)channel, *channel_mem_size, shm_fd,
                              *is_posix_shm, channel_name);
    *channel_mem_size = 0;
    // shm_fd is set to -1 by __machnet_channel_destroy().
    return NULL;
  }

  return channel;
}

static inline __attribute__((always_inline)) uint32_t __machnet_channel_enqueue(
    const MachnetChannelCtx_t *ctx, unsigned int n,
    const MachnetRingSlot_t *bufs) {
  assert(ctx != NULL);
  jring_t *machnet_ring = __machnet_channel_machnet_ring(ctx);

  return jring_enqueue_bulk(machnet_ring, bufs, n, NULL);
}

// #ifdef __cplusplus
// }
// #endif
#endif  // SRC_EXT_MACHNET_PRIVATE_H_
