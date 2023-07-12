#ifndef SRC_EXT_NSAAS_COMMON_H_
#define SRC_EXT_NSAAS_COMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A NSaaSChannelHeader is always constructed at the beginning (offset 0), of a
 * shared memory region that is going to be used for the communicaton between
 * the stack and one (or more in the case of a control channel) applications.
 *
 *     An NSaaS shared memory channel is being used as the dataplane for
 *     high-performance message communication between an application and the
 *     stack. The memory layout is as follows:
 *
 *
 *     [NSaaSChannelHeader]
 *     [NSaaSChannelStats]
 *     [Ring0: Stack->Application]
 *     [Ring1: Application->Stack]
 *     [Ring2: FreeBuffers]
 *     [HUGE_PAGE_2M_SIZE aligned]
 *     [Buf#0]
 *     [Buf#1]
 *     [...]
 *     [Buf#N]
 *
 *     Ring0 is used for communicating received messages from the stack to the
 *     application, and Ring1 for the opposite direction.
 *     Ring2 serves as the global pool of buffers.
 */

#include <assert.h>
#include <fcntl.h> /* For O_* constants */
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */

#include "jring.h"

#define KB (1 << 10)
#define MB (KB * KB)
#define CACHE_LINE_SIZE 64
#define PAGE_SIZE (4 * KB)
#define HUGE_PAGE_2M_SIZE (2 * MB)
#define NSAAS_MSG_MAX_LEN (8 * MB)

#ifndef likely
#define likely(x) __builtin_expect((x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif

#define __DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#define ALIGN_TO_PAGE_SIZE(x, _pagesz) (((x) + _pagesz - 1) & ~(_pagesz - 1))

typedef unsigned char uchar_t;
typedef uint32_t NSaaSRingSlot_t;
static_assert(sizeof(NSaaSRingSlot_t) % 4 == 0,
              "NSaaSRingSlot_t must be 32-bit aligned");

// This is the abstraction of a network flow for the applications. It is
// equivalent to the 5-tuple, with just the protocol missing (UDP is always
// assumed). This structure is used to indicate the sender or receiver of a
// message (depending on the direction). Equivalent to `struct sockaddr_in'.
struct NSaaSNetFlow {
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
};
typedef struct NSaaSNetFlow NSaaSNetFlow_t;

struct NSaaSListenerInfo {
  uint32_t ip;
  uint16_t port;
  uint16_t reserved;
};
typedef struct NSaaSListenerInfo NSaaSListenerInfo_t;

struct NSaaSChannelDataCtx {
  size_t stats_ofs;
  size_t ctrl_sq_ring_ofs;
  size_t ctrl_cq_ring_ofs;
  size_t nsaas_ring_ofs;
  size_t app_ring_ofs;
  size_t buf_ring_ofs;
  size_t buf_pool_ofs;
  size_t buf_pool_mask;
  uint32_t buf_size;
  uint32_t buf_mss;
} __attribute__((aligned(CACHE_LINE_SIZE)));
typedef struct NSaaSChannelDataCtx NSaaSChannelDataCtx_t;

struct NSaaSChannelCtrlCtx {
  // Mutex for protecting the control queue.
  size_t req_id;
} __attribute__((aligned(CACHE_LINE_SIZE)));
typedef struct NSaaSChannelCtrlCtx NSaaSChannelCtrlCtx_t;

/**
 * The `NSaaSChannelCtx' holds all the metadata information (context) of an
 * NSaaS Channel.
 *
 * It is always located at the beginning of the shared memory area.
 */
struct NSaaSChannelCtx {
#define NSAAS_CHANNEL_CTX_MAGIC 0xA5A5A5A5
  uint32_t magic;  // Magic value tagged after initialization.
#define NSAAS_CHANNEL_VERSION 0x01
  uint16_t version;
  uint64_t size;  // Size of the Channel's memory, including this context.
#define NSAAS_CHANNEL_NAME_MAX_LEN 256
  char name[NSAAS_CHANNEL_NAME_MAX_LEN];
  NSaaSChannelCtrlCtx_t ctrl_ctx;  // Control channel's specific metadata.
  NSaaSChannelDataCtx_t data_ctx;  // Dataplane channel's specific metadata.
} __attribute__((aligned(CACHE_LINE_SIZE)));
typedef struct NSaaSChannelCtx NSaaSChannelCtx_t;

struct NSaaSChannelAppStats {
  uint64_t tx_msg_drops;
  uint64_t tx_msg_success;
  uint64_t tx_bytes_success;
  uint64_t reserved[5];
};
typedef struct NSaaSChannelAppStats NSaaSChannelAppStats_t;

/**
 * NSaaS channel statistics.
 *
 * TODO(ilias): At the moment it contains statistics for the application side.
 */
struct NSaaSChannelStats {
  NSaaSChannelAppStats_t a_stats;
} __attribute__((aligned(CACHE_LINE_SIZE)));
typedef struct NSaaSChannelStats NSaaSChannelStats_t;

struct NSaaSCtrlQueueEntry {
  uint64_t id;
#define NSAAS_CTRL_OP_CREATE_FLOW 0x0001
#define NSAAS_CTRL_OP_DESTROY_FLOW 0x0002
#define NSAAS_CTRL_OP_LISTEN 0x0003
#define NSAAS_CTRL_OP_STATUS 0x0004;
  uint32_t opcode;
#define NSAAS_CTRL_STATUS_OK 0x0000
#define NSAAS_CTRL_STATUS_ERROR 0x0001
  uint16_t status;
  union {
    NSaaSNetFlow_t flow_info;
    NSaaSListenerInfo_t listener_info;
  };
};
typedef struct NSaaSCtrlQueueEntry NSaaSCtrlQueueEntry_t;
static_assert(sizeof(NSaaSCtrlQueueEntry_t) % 4 == 0,
              "NSaaSCtrlSqEntry_t must be 32-bit aligned");

/**
 * Message Buffer Header: This header is carried at the beginning of every
 * buffer of an NSaaS dataplane channel.
 */
struct NSaaSMsgBuf {
#define NSAAS_MSGBUF_MAGIC 0xB6B6B6B6
  const uint32_t magic;  // Magic value tagged after initialization.
  const uint32_t index;  // Index of the buffer in the buffer pool.
  const uint32_t size;   // Absolute static size of the buffer.
  const uintptr_t iova;  // IOVA address of the buffer.
#define NSAAS_MSGBUF_FLAGS_SYN (1 << 0)
#define NSAAS_MSGBUF_FLAGS_SG (1 << 1)
#define NSAAS_MSGBUF_FLAGS_FIN (1 << 2)
#define NSAAS_MSGBUF_FLAGS_CHAIN (1 << 3)
#define NSAAS_MSGBUF_NOTIFY_DELIVERY (1 << 7)
  uint8_t flags;
  NSaaSNetFlow_t flow;  // Network flow info.
  uint32_t msg_len;     // This is the total length of the message (could be
                     // larger than the buffer size). Set in the first buffer.
  uint32_t data_len;  // Length of the data in this buffer.
  uint32_t data_ofs;  // Offset of the data in this buffer.
  // If multi-buffer message (SG), next points to next buffer index.
  uint32_t next;
  // If multi-buffer message (SG), last points to the last buffer index.
  // This is only set in the first buffer of the message.
  uint32_t last;
  uint32_t reserved1[1];
} __attribute__((aligned(CACHE_LINE_SIZE)));
typedef struct NSaaSMsgBuf NSaaSMsgBuf_t;
#define NSAAS_MSGBUF_SPACE_RESERVED (sizeof(NSaaSMsgBuf_t))
static_assert(NSAAS_MSGBUF_SPACE_RESERVED == CACHE_LINE_SIZE,
              "NSaaSMsgBuf_t is not aligned");
#define NSAAS_MSGBUF_HEADROOM_MAX (2 * CACHE_LINE_SIZE)

static inline __attribute__((always_inline)) void __nsaas_channel_buf_init(
    NSaaSMsgBuf_t *buf) {
  // Do not set the magic here. Should be set in initialization only.
  buf->flags = 0;
  buf->flow.src_ip = 0;
  buf->flow.dst_ip = 0;
  buf->flow.src_port = 0;
  buf->flow.dst_port = 0;
  buf->msg_len = 0;
  buf->data_len = 0;
  buf->data_ofs = NSAAS_MSGBUF_HEADROOM_MAX;
  buf->next = UINT32_MAX;
  buf->last = UINT32_MAX;
}

/**
 * Get a pointer to an arbitrary offset of NSaaS memory area.
 *
 * @param ctx                Channel's context.
 * @param offset             Desired offset.
 * @return                   A `uchar_t' pointer to the requested offset.
 */
static inline __attribute__((always_inline)) uchar_t *__nsaas_channel_mem_ofs(
    const NSaaSChannelCtx_t *ctx, size_t offset) {
  // We need to de-const here.
  return (__DECONST(uchar_t *, ctx) + offset);
}

/**
 * Get a pointer to the control submission queue. (Application->NSaaS)
 *
 * @param ctx                Channel's context.
 * @return                   A pointer to the control submission queue.
 */
static inline __attribute__((always_inline)) jring_t *
__nsaas_channel_ctrl_sq_ring(const NSaaSChannelCtx_t *ctx) {
  return (jring_t *)__nsaas_channel_mem_ofs(ctx,
                                            ctx->data_ctx.ctrl_sq_ring_ofs);
}

/**
 * Get a pointer to the control completion queue. (NSaaS -> Application)
 *
 * @param ctx                Channel's context.
 * @return                   A pointer to the control completion queue.
 */
static inline __attribute__((always_inline)) jring_t *
__nsaas_channel_ctrl_cq_ring(const NSaaSChannelCtx_t *ctx) {
  return (jring_t *)__nsaas_channel_mem_ofs(ctx,
                                            ctx->data_ctx.ctrl_cq_ring_ofs);
}

/**
 * Get a pointer to the `NSaaS' ring (NSaaS->Application).
 *
 * @param ctx                Channel's context.
 * @return                   A pointer to the NSaaS Ring.
 */
static inline __attribute__((always_inline)) jring_t *
__nsaas_channel_nsaas_ring(const NSaaSChannelCtx_t *ctx) {
  return (jring_t *)__nsaas_channel_mem_ofs(ctx, ctx->data_ctx.nsaas_ring_ofs);
}

/**
 * Get a pointer to the `App' ring (Application->NSaaS).
 *
 * @param ctx                Channel's context.
 * @return                   A pointer to the Application Ring.
 */
static inline __attribute__((always_inline)) jring_t *__nsaas_channel_app_ring(
    const NSaaSChannelCtx_t *ctx) {
  return (jring_t *)__nsaas_channel_mem_ofs(ctx, ctx->data_ctx.app_ring_ofs);
}

/**
 * Get a pointer to the `MsgBuf' ring (allocator pool).
 *
 * @param ctx                Channel's context.
 * @return                   A pointer to the MsgBuf Ring.
 */
static inline __attribute__((always_inline)) jring_t *__nsaas_channel_buf_ring(
    const NSaaSChannelCtx_t *ctx) {
  return (jring_t *)__nsaas_channel_mem_ofs(ctx, ctx->data_ctx.buf_ring_ofs);
}

/**
 * Get a pointer to the end of the `NSaaS' channel.
 * @param ctx                Channel's context.
 * @return                   A pointer to the end of the NSaaS channel.
 */
static inline __attribute__((always_inline)) uchar_t *__nsaas_channel_end(
    const NSaaSChannelCtx_t *ctx) {
  return __nsaas_channel_mem_ofs(ctx, ctx->size);
}

/**
 * Get a pointer to the beginning of the buffer pool (i.e., the first MsgBuf).
 * @param ctx                Channel's context.
 * @return                   A pointer to the beginning of the buffer pool.
 */
static inline __attribute__((always_inline)) uchar_t *__nsaas_channel_buf_pool(
    const NSaaSChannelCtx_t *ctx) {
  return (uchar_t *)__nsaas_channel_mem_ofs(ctx, ctx->data_ctx.buf_pool_ofs);
}

static inline __attribute__((always_inline)) size_t
__nsaas_channel_buf_pool_size(const NSaaSChannelCtx_t *ctx) {
  return ctx->data_ctx.buf_pool_mask * ctx->data_ctx.buf_size;
}

/**
 * Get a pointer to the begining of the NSaaS MsgBuf at a particular index.
 *
 * @param ctx                Channel's context.
 * @param index              Index of the buffer.
 * @return                   A pointer to the beginning of the MsgBuf.
 */
static inline __attribute__((always_inline)) NSaaSMsgBuf_t *__nsaas_channel_buf(
    const NSaaSChannelCtx_t *ctx, uint32_t index) {
  size_t buf_ofs = ctx->data_ctx.buf_pool_ofs + index * ctx->data_ctx.buf_size;
  return (NSaaSMsgBuf_t *)__nsaas_channel_mem_ofs(ctx, buf_ofs);
}

/**
 * Given a pointer to the channel context, and a pointer to a `MsgBuf' return
 * its index.
 *
 * @param ctx                Channel's context.
 * @param buf                A pointer to the `MsgBuf' buffer
 * @return                   (NSaaSRingSlot_t) Index of the buffer
 */
static inline __attribute__((always_inline)) NSaaSRingSlot_t
__nsaas_channel_buf_index(const NSaaSChannelCtx_t *ctx,
                          const NSaaSMsgBuf_t *buf) {
  assert(ctx != NULL);
  assert(buf != NULL);
  const uint32_t buf_size = ctx->data_ctx.buf_size;
  const size_t buf_pool_ofs = ctx->data_ctx.buf_pool_ofs;
  NSaaSRingSlot_t index =
      ((uintptr_t)buf - (uintptr_t)__nsaas_channel_mem_ofs(ctx, buf_pool_ofs)) /
      buf_size;
  assert(index <= ctx->data_ctx.buf_pool_mask);
  return index;
}

/**
 * Get a pointer to the base of a particular buffer. This is the minimum address
 * that is valid for holding actual data. Space before this address is reserved.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   A pointer to the begining of the data.
 */
static inline __attribute__((always_inline)) uchar_t *__nsaas_channel_buf_base(
    const NSaaSMsgBuf_t *buf) {
  return ((uchar_t *)buf + NSAAS_MSGBUF_SPACE_RESERVED);
}

/**
 * Get a pointer to the begining of data of a particular buffer.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   A pointer to the data of the MsgBuf.
 */
static inline __attribute__((always_inline)) uchar_t *__nsaas_channel_buf_data(
    const NSaaSMsgBuf_t *buf) {
  return __nsaas_channel_buf_base(buf) + buf->data_ofs;
}

/**
 * Get a pointer at a certain offset of the data of a particular buffer.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   A pointer at the requested offset of the data.
 */
static inline __attribute__((always_inline)) uchar_t *
__nsaas_channel_buf_data_ofs(const NSaaSMsgBuf_t *buf, uint32_t ofs) {
  return __nsaas_channel_buf_data(buf) + ofs;
}

/**
 * Return the space availabe at the beginning of the buffer.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   Number of bytes available.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_buf_headroom(const NSaaSMsgBuf_t *buf) {
  assert(buf != NULL);
  return (buf->data_ofs);
}

/**
 * Return the space availabe at the end of the buffer.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   Number of bytes available.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_buf_tailroom(const NSaaSMsgBuf_t *buf) {
  assert(buf != NULL);
  return (buf->size - buf->data_ofs - buf->data_len);
}

/**
 * @brief Buffer data length. This is the length of the current data in the
 * buffer.
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   Total size of the buffer.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_buf_data_len(const NSaaSMsgBuf_t *buf) {
  assert(buf != NULL);
  return (buf->data_len);
}

/**
 * @brief Usable buffer size.
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @return                   Total size of the buffer.
 */
static inline __attribute__((always_inline)) uint32_t __nsaas_channel_buf_size(
    const NSaaSMsgBuf_t *buf) {
  assert(buf != NULL);
  return (buf->size);
}

/**
 * Prepend data of size `len' to a `MsgBuf' and return a pointer to the
 * beginning of the newly added area.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @param len                # of bytes to prepend.
 * @return                   A pointer to the appropriate data offset on
 *                           success, NULL on failure.
 */
static inline __attribute__((always_inline)) uchar_t *
__nsaas_channel_buf_prepend(NSaaSMsgBuf_t *buf, uint32_t len) {
  if (unlikely(__nsaas_channel_buf_headroom(buf) < len)) return NULL;

  buf->data_ofs -= len;
  buf->data_len += len;
  return __nsaas_channel_buf_data(buf);
}

/**
 * Extend the data length of the `MsgBuf' by `len' and return a pointer to the
 * beginning of the newly added area.
 *
 * @param buf                A pointer to the `MsgBuf' buffer.
 * @param len                # of bytes to append.
 * @return                   A pointer to the appropriate data offset on
 *                           success, NULL on failure.
 */
static inline __attribute__((always_inline)) uchar_t *
__nsaas_channel_buf_append(NSaaSMsgBuf_t *buf, uint32_t len) {
  if (unlikely(__nsaas_channel_buf_tailroom(buf) < len)) return NULL;

  uchar_t *data_ptr = __nsaas_channel_buf_data_ofs(buf, buf->data_len);
  buf->data_len += len;
  return data_ptr;
}

/**
 * Allocate a number of `MsgBuf' buffers from the channel's pool.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of buffers to allocate.
 * @param indices            Pointer to an array that can hold at least `n'
 *                           `NSaaSRingSlot_t'-sized objects to store the
 *                           allocated buffer indexes.
 * @param bufs               (Optional: NULL) Pointer to an array that can hold
 *                           at least `n' pointers to `NSaaSMsgBuf_t' objects to
 *                           store the allocated buffer pointers.
 * @return                   Number of buffers allocated, either 0 or `n'.
 */
static inline __attribute__((always_inline)) unsigned int
__nsaas_channel_buf_alloc_bulk(const NSaaSChannelCtx_t *ctx, uint32_t n,
                               NSaaSRingSlot_t *indices, NSaaSMsgBuf_t **bufs) {
  assert(ctx != NULL);
  assert(indices != NULL);

  jring_t *buf_ring = __nsaas_channel_buf_ring(ctx);

  // Both sides can allocate buffers concurrently, so use directly the
  // multi-consumer function.
  uint32_t ret = jring_mc_dequeue_bulk(buf_ring, indices, n, NULL);
  for (uint32_t i = 0; i < ret; i++) {
    assert(indices[i] < buf_ring->capacity);
    // Initialize all buffers in the allocated batch.
    NSaaSMsgBuf_t *msg_buf = __nsaas_channel_buf(ctx, indices[i]);
    __nsaas_channel_buf_init(msg_buf);
    if (bufs != NULL) bufs[i] = msg_buf;
  }

  return ret;
}

/**
 * Release a number of `MsgBuf' buffers back to the channel's pool.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of buffers to release.
 * @param bufs               Pointer to an array of `n' `NSaaSRingSlot_t'-sized
 *                           objects that contain the indices of the buffers to
 *                           be freed.
 * @return                   Number of buffers freed, either 0 or `n'.
 *                           NOTE: With correct use, this fuction must always
 *                           succeed (i.e, return `n').
 */
static inline __attribute__((always_inline)) unsigned int
__nsaas_channel_buf_free_bulk(const NSaaSChannelCtx_t *ctx, uint32_t n,
                              const NSaaSRingSlot_t *bufs) {
  assert(ctx != NULL);
  assert(bufs != NULL);

  jring_t *buf_ring = __nsaas_channel_buf_ring(ctx);

#ifndef NDEBUG
  for (uint32_t i = 0; i < n; i++) {
    assert(bufs[i] < buf_ring->capacity);
  }
#endif
  // Both sides can release buffers concurrently, so use directly the
  // multi-consumer function.
  return jring_mp_enqueue_bulk(buf_ring, bufs, n, NULL);
}

/**
 * Return the number of free buffers in the channel's pool.
 *
 * @param ctx                Channel's context.
 * @return                   Number of items free.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_buffers_avail(const NSaaSChannelCtx_t *ctx) {
  assert(ctx != NULL);

  jring_t *buf_ring = __nsaas_channel_buf_ring(ctx);
  return jring_count(buf_ring);
}

/**
 * Return the number of pending items in the NSaaS ring.
 *
 * @param ctx                Channel's context.
 * @return                   Number of items pending.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_nsaas_ring_pending(const NSaaSChannelCtx_t *ctx) {
  assert(ctx != NULL);

  jring_t *nsaas_ring = __nsaas_channel_nsaas_ring(ctx);
  return jring_count(nsaas_ring);
}

/**
 * Return the number of pending items in the application ring.
 *
 * @param ctx                Channel's context.
 * @return                   Number of items pending.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_app_ring_pending(const NSaaSChannelCtx_t *ctx) {
  assert(ctx != NULL);

  jring_t *app_ring = __nsaas_channel_app_ring(ctx);
  return jring_count(app_ring);
}

/**
 * @brief Enqueue a number of `NSaaSQueueEntry' objects in the control
 * Submission Queue.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of entries to enqueue.
 * @param op                 Pointer to an array of `n' `NSaaSQueueEntry_t'
 * @return                 Number of entries enqueued, either 0 or `n'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_ctrl_sq_enqueue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                const NSaaSCtrlQueueEntry_t *op) {
  assert(ctx != NULL);
  assert(n > 0);
  assert(op != NULL);

  jring_t *ctrl_sq = __nsaas_channel_ctrl_sq_ring(ctx);
  return jring_enqueue_bulk(ctrl_sq, op, n, NULL);
}

/**
 * @brief Dequeue up to `n' `NSaaSQueueEntry' objects from the Submission Queue.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of entries to dequeue at most.
 * @param op                 Pointer to an array of `n' `NSaaSQueueEntry_t'
 * @return                   Number of entries dequeued, from 0 or `n'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_ctrl_sq_dequeue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                NSaaSCtrlQueueEntry_t *op) {
  assert(ctx != NULL);
  assert(n > 0);
  assert(op != NULL);

  jring_t *ctrl_sq = __nsaas_channel_ctrl_sq_ring(ctx);
  return jring_dequeue_burst(ctrl_sq, op, n, NULL);
}

/**
 * @brief Enqueue a number of `NSaaSQueueEntry' objects in the control
 * Completion Queue.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of entries to enqueue.
 * @param op                 Pointer to an array of `n' `NSaaSQueueEntry_t'.
 * @return                   Number of entries enqueued, either 0 or `n'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_ctrl_cq_enqueue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                const NSaaSCtrlQueueEntry_t *op) {
  assert(ctx != NULL);
  assert(n > 0);
  assert(op != NULL);

  jring_t *ctrl_cq = __nsaas_channel_ctrl_cq_ring(ctx);
  return jring_enqueue_bulk(ctrl_cq, op, n, NULL);
}

/**
 * @brief Dequeue up to `n' `NSaaSQueueEntry' objects from the Completion Queue.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of entries to dequeue at most.
 * @param op                 Pointer to an array of `n' `NSaaSQueueEntry_t'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_ctrl_cq_dequeue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                NSaaSCtrlQueueEntry_t *op) {
  assert(ctx != NULL);
  assert(n > 0);
  assert(op != NULL);

  jring_t *ctrl_cq = __nsaas_channel_ctrl_cq_ring(ctx);
  return jring_dequeue_burst(ctrl_cq, op, n, NULL);
}

/**
 * Enqueue a number of messages/`MsgBuf' buffers sent from the application to
 * the NSaaS.
 * NOTE: For messages that span over multiple buffers, the caller is respnsible
 * can construct a "linked list" using the appropriate fields in `MsgBuf_t'.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of buffers to enqueue.
 * @param bufs               Pointer to an array of `n' `NSaaSRingSlot_t'-sized
 *                           objects that contain the indices of the buffers to
 *                           be sent.
 * @return                   Number of buffers sent, either 0 or `n'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_app_ring_enqueue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                 const NSaaSRingSlot_t *bufs) {
  assert(ctx != NULL);
  assert(bufs != NULL);

  jring_t *app_ring = __nsaas_channel_app_ring(ctx);

  // Multiple application threads might be enqueuing concurrently.
  return jring_mp_enqueue_bulk(app_ring, bufs, n, NULL);
}

/**
 * Dequeue a number of pending messages/`MsgBuf' buffers destined for the
 * application.
 *
 * @param ctx                Channel's context.
 * @param n                  Maximum number of `MsgBuf_t' to dequeue.
 * @param bufs               Pointer to an array that can hold up to `n'
 *                           `NSaaSRingSlot_t'-sized objects that contain the
 *                           indices of the received buffers.
 * @return                   Number of buffers received, ranging [0, n].
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_app_ring_dequeue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                 NSaaSRingSlot_t *bufs) {
  jring_t *app_ring = __nsaas_channel_app_ring(ctx);

  // Multiple application threads might be enqueuing concurrently.
  // Burst deque elements.
  return jring_mc_dequeue_burst(app_ring, bufs, n, NULL);
}

/**
 * Enqueue a number of messages/`MsgBuf' buffers sent from NSaaS to the
 * application.
 * NOTE: For messages that span over multiple buffers,
 * the caller is respnsible can construct a "linked list" using the appropriate
 * fields in `MsgBuf_t'.
 *
 * @param ctx                Channel's context.
 * @param n                  Number of buffers to enqueue.
 * @param bufs               Pointer to an array of `n' `NSaaSRingSlot_t'-sized
 *                           objects that contain the indices of the buffers to
 *                           be sent.
 * @return                   Number of buffers sent, either 0 or `n'.
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_nsaas_ring_enqueue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                   const NSaaSRingSlot_t *bufs) {
  assert(ctx != NULL);
  assert(bufs != NULL);

  jring_t *nsaas_ring = __nsaas_channel_nsaas_ring(ctx);

  // Multiple application threads might be enqueuing concurrently.
  return jring_mp_enqueue_bulk(nsaas_ring, bufs, n, NULL);
}

/**
 * Dequeue a number of pending messages/`MsgBuf' buffers destined for the
 * NSaaS.
 *
 * @param ctx                Channel's context.
 * @param n                  Maximum number of `MsgBuf_t' to dequeue.
 * @param bufs               Pointer to an array that can hold up to `n'
 *                           `NSaaSRingSlot_t'-sized objects that contain the
 *                           indices of the received buffers.
 * @return                   Number of buffers received, ranging [0, n].
 */
static inline __attribute__((always_inline)) uint32_t
__nsaas_channel_nsaas_ring_dequeue(const NSaaSChannelCtx_t *ctx, unsigned int n,
                                   NSaaSRingSlot_t *bufs) {
  jring_t *nsaas_ring = __nsaas_channel_nsaas_ring(ctx);

  // Multiple application threads might be enqueuing concurrently.
  // Burst deque elements.
  return jring_mc_dequeue_burst(nsaas_ring, bufs, n, NULL);
}

#ifdef __cplusplus
}
#endif

#endif  // SRC_EXT_NSAAS_COMMON_H_
