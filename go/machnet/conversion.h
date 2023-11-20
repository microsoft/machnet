// Convert Go Pointers to void* before passing to C

#ifndef GO_MACHNET_CONVERSION_H_
#define GO_MACHNET_CONVERSION_H_

#include <machnet.h>

int __machnet_sendmsg_go(const MachnetChannelCtx_t* ctx, MachnetIovec_t msg_iov,
                         long msg_iovlen, MachnetFlow_t flow) {
  MachnetMsgHdr_t msghdr;
  msghdr.msg_size = msg_iov.len;

  msghdr.flow_info.dst_ip = flow.dst_ip;
  msghdr.flow_info.src_ip = flow.src_ip;
  msghdr.flow_info.dst_port = flow.dst_port;
  msghdr.flow_info.src_port = flow.src_port;

  msghdr.msg_iov = &msg_iov;
  msghdr.msg_iovlen = msg_iovlen;

  return machcnet_sendmsg(ctx, &msghdr);
}

MachnetFlow_t __machnet_recvmsg_go(const MachnetChannelCtx_t* ctx,
                                   MachnetIovec_t msg_iov, long msg_iovlen) {
  MachnetMsgHdr_t msghdr;
  msghdr.msg_iov = &msg_iov;
  msghdr.msg_iovlen = msg_iovlen;
  int ret = machnet_recvmsg(ctx, &msghdr);

  if (ret > 0) {
    return msghdr.flow_info;
  } else {
    MachnetFlow_t flow;
    flow.dst_ip = 0;
    flow.src_ip = 0;
    flow.dst_port = 0;
    flow.src_port = 0;
    return flow;
  }
}

MachnetFlow_t __machnet_recv_go(const MachnetChannelCtx_t* ctx, void* buf,
                                size_t len, ssize_t* recv_len) {
  MachnetFlow_t flow;
  ssize_t num_bytes = machnet_recv(ctx, buf, len, &flow);

  if (num_bytes > 0) {
    *recv_len = num_bytes;
    return flow;
  } else {
    flow.dst_ip = 0;
    flow.src_ip = 0;
    flow.dst_port = 0;
    flow.src_port = 0;

    if (num_bytes < 0) {
      *recv_len = num_bytes;
    } else {
      *recv_len = 0;
    }
    return flow;
  }
}

int __machnet_connect_go(MachnetChannelCtx_t* ctx, const char* local_ip,
                         const char* remote_ip, uint16_t remote_port,
                         MachnetFlow_t* flow) {
  return machnet_connect(ctx, local_ip, remote_ip, remote_port, flow);
}

int __machnet_listen_go(MachnetChannelCtx_t* ctx, const char* local_ip,
                        uint16_t port) {
  return machnet_listen(ctx, local_ip, port);
}

MachnetFlow_t* __machnet_init_flow() {
  MachnetFlow_t* flow = (MachnetFlow_t*)malloc(sizeof(MachnetFlow_t));
  flow->dst_ip = 0;
  flow->src_ip = 0;
  flow->dst_port = 0;
  flow->src_port = 0;
  return flow;
}

void __machnet_destroy_flow(MachnetFlow_t* flow) { free(flow); }

#endif
