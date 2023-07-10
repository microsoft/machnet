// Convert Go Pointers to void* before passing to C

#ifndef CONVERSION_H
#define CONVERSION_H

#include <nsaas.h>

int __nsaas_sendmsg_go(const NSaaSChannelCtx_t* ctx, NSaaSIovec_t msg_iov,
                       long msg_iovlen, NSaaSNetFlow_t flow) {
  NSaaSMsgHdr_t msghdr;
  msghdr.msg_size = msg_iov.len;

  msghdr.flow_info.dst_ip = flow.dst_ip;
  msghdr.flow_info.src_ip = flow.src_ip;
  msghdr.flow_info.dst_port = flow.dst_port;
  msghdr.flow_info.src_port = flow.src_port;
  
  msghdr.msg_iov = &msg_iov;
  msghdr.msg_iovlen = msg_iovlen;

  return __nsaas_sendmsg(ctx, &msghdr);
}

NSaaSNetFlow_t __nsaas_recvmsg_go(const NSaaSChannelCtx_t* ctx,
                                  NSaaSIovec_t msg_iov, long msg_iovlen) {
  NSaaSMsgHdr_t msghdr;
  msghdr.msg_iov = &msg_iov;
  msghdr.msg_iovlen = msg_iovlen;
  int ret = __nsaas_recvmsg(ctx, &msghdr);

  if (ret > 0) {
    return msghdr.flow_info;
  } else {
    NSaaSNetFlow_t flow;
    flow.dst_ip = 0;
    flow.src_ip = 0;
    flow.dst_port = 0;
    flow.src_port = 0;
    return flow;
  }
}

int __nsaas_connect_go(NSaaSChannelCtx_t* ctx, uint32_t local_ip,
                       uint32_t remote_ip, uint16_t remote_port,
                       NSaaSNetFlow_t* flow) {
  return nsaas_connect(ctx, local_ip, remote_ip, remote_port, flow);
}

int __nsaas_listen_go(NSaaSChannelCtx_t* ctx, uint32_t local_ip,
                      uint16_t port) {
  return nsaas_listen(ctx, local_ip, port);
}

NSaaSNetFlow_t* __nsaas_init_flow() {
  NSaaSNetFlow_t* flow = (NSaaSNetFlow_t*)malloc(sizeof(NSaaSNetFlow_t));
  flow->dst_ip = 0;
  flow->src_ip = 0;
  flow->dst_port = 0;
  flow->src_port = 0;
  return flow;
}

void __nsaas_destroy_flow(NSaaSNetFlow_t* flow) { free(flow); }

#endif