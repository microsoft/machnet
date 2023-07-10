/**
 * @file  nsaas.h
 * @brief This is the header file for the NSaaS interface library, which
 * provides applications a way to interact with the NSaaS service on their
 * machine.
 */

#ifndef SRC_EXT_NSAAS_H_
#define SRC_EXT_NSAAS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "nsaas_common.h"

#define MIN(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
  })

// This structure is being used as a descriptor to SG data that
// constitute a message. Resembles `struct iovec'.
struct NSaaSIovec {
  void *base;
  size_t len;
};
typedef struct NSaaSIovec NSaaSIovec_t;

// This structure is being used as a descriptor to a message.
// Resembles `struct msghdr', but a few adjustments.
// - `msg_size' is the total size of the message payload.
// - `peer_addr' is the address of the network peer that is the recipient or
//    sender of the message (depending on the direction).
// - `msg_iov' is a vector of `msg_iovlen' `NSaaSIovec_t' structures.
// - `msg_iovlen' is the number of `NSaaSIovec_t' structures in `msg_iov'.
// - `flags' is the message flags.
struct NSaaSMsgHdr {
  uint32_t msg_size;
  NSaaSNetFlow_t flow_info;
  NSaaSIovec_t *msg_iov;
  size_t msg_iovlen;
  uint16_t flags;
};
typedef struct NSaaSMsgHdr NSaaSMsgHdr_t;

// This socket is the persistent connection between the  application and the
// NSaaS controller.
extern int g_ctrl_socket;

/**
 * @brief Initializes the NSaaS library for the application, which is used
 * to interact with the NSaaS service on the machine.
 *
 * @return 0 on success, -1 on failure.
 */
int nsaas_init();

/**
 * @brief NOT part of the public API.
 * 
 * This is a helper function used to bind to a shared memory segment from
 * the application. The NSaaS controller is going to hand over an open file
 * descriptor to the appropriate shared memory segment. Internally, this
 * function is resolving the size of the shared memory segment, and memory maps
 * it to the process address space of the caller (application).
 *
 * @param shm_fd Open file descriptor for the shared memory segment.
 * @param channel_size Pointer to a `size_t` variable that will be filled with
 * the size of the channel. This is optional and can be `NULL`.
 * @return A pointer to the mapped channel on success, `NULL` otherwise.
 */
NSaaSChannelCtx_t *nsaas_bind(int shm_fd, size_t *channel_size);

/**
 * @brief Creates a new channel to the NSaaS controller and binds to it. A
 * channel is a logical entity between an application and the NSaaS service.
 * 
 * @return A pointer to the channel context on success, NULL otherwise.
 */
void *nsaas_attach();

/**
 * @brief Listens for incoming messages on a specific IP and port.
 * @param[in] channel The channel associated to the listener.
 * @param[in] ip The local IP address to listen on.
 * @param[in] port The local port to listen on.
 * @return 0 on success, -1 on failure.
 */
int nsaas_listen(void *channel_ctx, const char *local_ip, uint16_t port);

/**
 * @brief Creates a new connection to a remote peer.
 * @param[in] channel     The channel associated with the connection.
 * @param[in] local_ip    The local IP address.
 * @param[in] remote_ip   The remote IP address.
 * @param[in] remote_port The remote port.
 * @param[out] flow       A pointer to a `NSaaSNetFlow_t` structure that will be
 *                        filled by the function upon success.
 * @return  0 on success, -1 on failure. `flow` is filled with the flow
 * information on success.
 */
int nsaas_connect(void *channel_ctx, const char *local_ip,
                  const char *remote_ip, uint16_t remote_port,
                  NSaaSNetFlow_t *flow);

/**
 * Enqueue one message for transmission to a remote peer over the network.
 *
 * @param[in] channel_ctx The NSaaS channel context
 * @param[in] flow The pre-created flow to the remote peer
 * @param[in] buf The data buffer to send to the remote peer
 * @param[in] len The length of the data buffer in bytes
 */
int nsaas_send(const void *channel_ctx, NSaaSNetFlow_t flow, const void *buf,
               size_t len);

/**
 * This function enqueues one message for transmission to a remote peer over
 * the network. The application needs to provide the destination's (remote
 * peer) address. NSaaS is responsible for end-to-end encrypted, reliable
 * delivery of each message to the relevant receiver. This function supports
 * SG collection of a message's buffers from the application's address
 * space.
 *
 * @param[in] channel_ctx        The NSaaS channel context
 * @param[in] msghdr             An `NSaaSMsgHdr' descriptor
 * @return                   0 on success, -1 on failure
 */
int nsaas_sendmsg(const void *channel_ctx, const NSaaSMsgHdr_t *msghdr);

/**
 * This function sends one or more messages to a remote peer over the network.
 * The application needs to provide the destination's (remote peer) address.
 * NSaaS is responsible for end-to-end encrypted, reliable delivery of each
 * message to the relevant receiver. This function supports SG collection of a
 * message's buffers from the application's address space.
 *
 * @param[in] channel_ctx        The NSaaS channel context
 * @param[in] msghdr_iovec       An array of `NSaaSMsgHdr' descriptors, each one
 *                               describing a standalone TX message.
 * @param[in] vlen               Length of the `msghdr_iovec' array (number of
 *                               messages to be sent).
 * @return                       # of messages sent.
 */
int nsaas_sendmmsg(const void *channel_ctx, const NSaaSMsgHdr_t *msghdr_iovec,
                   int vlen);

/**
 * Receive a pending message from some remote peer over the network.
 *
 * @param[in] channel_ctx The NSaaS channel context
 * @param[out] buf The data buffer to receive the message
 * @param[in] len The length of \p buf in bytes
 * @param[out] flow The flow information of the sender
 *
 * @return 0 if no message is available, -1 on failure, otherwise the number of
 * bytes received.
 */
ssize_t nsaas_recv(const void *channel_ctx, void *buf, size_t len,
                   NSaaSNetFlow_t *flow);

/**
 * This function receives a pending message (destined to the application) from
 * the NSaaS Channel. The application is responsible from providing an
 * appropriate msghdr, which describes the locations of the buffers (SG is
 * supported) to which the message should be copied to. The sender's network
 * information can be found in the `flow_info` field of the msghdr.
 *
 * @param[in] ctx                The NSaaS channel context
 * @param[in, out] msghdr        An `NSaaSMsgHdr' descriptor. The application
 *                               needs to fill in the `msg_iov` and `msg_iovlen`
 *                               members, which describe the locations of the
 *                               buffers to which the message should be copied
 *                               to. The `flow_info` member is set by NSaaS to
 *                               indicate the flow that the message belongs to.
 * @return                       0 if no pending message, 1 if a message is
 *                               received, -1 on failure
 */
int nsaas_recvmsg(const void *channel_ctx, NSaaSMsgHdr_t *msghdr);

#ifdef __cplusplus
}
#endif

#endif  // SRC_EXT_NSAAS_H_
