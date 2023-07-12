package nsaas

// #cgo LDFLAGS: -L${SRCDIR}/../../build/src/core -lcore -L${SRCDIR}/../../build/src/ext -lnsaas_shim -lrt -Wl,-rpath=${SRCDIR}/../../build/src/core:${SRCDIR}/../../build/src/ext -fsanitize=address
// #cgo CFLAGS: -I${SRCDIR}/../../src/ext -I${SRCDIR}/../../src/include -fsanitize=address
// #include <stdlib.h>
// #include "conversion.h"
// #include "../../src/ext/nsaas.h"
// #include "../../src/ext/nsaas_common.h"
import "C"
import (
	"strconv"
	"strings"
	"unsafe"
)

// Alternate Go Type Defs for C types
type NSaaSChannelCtx = C.NSaaSChannelCtx_t

// Define the NSaaSNetFlow struct
type NSaaSNetFlow struct {
	SrcIp   uint32
	DstIp   uint32
	SrcPort uint16
	DstPort uint16
}

// Helper function to convert a C.NSaaSNetFlow_t to a NSaaSNetFlow.
func convert_net_flow_go(c_flow *C.NSaaSNetFlow_t) NSaaSNetFlow {
	return NSaaSNetFlow{
		SrcIp:   (uint32)(c_flow.src_ip),
		DstIp:   (uint32)(c_flow.dst_ip),
		SrcPort: (uint16)(c_flow.src_port),
		DstPort: (uint16)(c_flow.dst_port),
	}
}

// Helper function to convert a NSaaSNetFlow to a C.NSaaSNetFlow_t.
func convert_net_flow_c(flow NSaaSNetFlow) C.NSaaSNetFlow_t {
	return C.NSaaSNetFlow_t{
		src_ip:   (C.uint)(flow.SrcIp),
		dst_ip:   (C.uint)(flow.DstIp),
		src_port: (C.ushort)(flow.SrcPort),
		dst_port: (C.ushort)(flow.DstPort),
	}
}

// Helper function to convert a IPv4 address string to a uint32.
func ipv4_str_to_uint32(ipv4_str string) uint32 {
	bytes := strings.Split(ipv4_str, ".")
	var ipv4_uint32 uint32 = 0
	for i := 0; i < 4; i++ {
		val, _ := strconv.Atoi(bytes[i])
		ipv4_uint32 |= uint32(val) << uint32(8*(3-i))
	}
	return ipv4_uint32
}

// Initialize the NSAAS shim.
func Init() int {
	ret := C.nsaas_init()
	return (int)(ret)
}

// Attach to the NSAAS shim.
// Returns a pointer to the channel context.
func Attach() *NSaaSChannelCtx {
	var c_ctx *C.NSaaSChannelCtx_t = (*C.NSaaSChannelCtx_t)(C.nsaas_attach())
	return (*NSaaSChannelCtx)(c_ctx)
}

// Connect to the remote host and port.
func Connect(ctx *NSaaSChannelCtx, local_ip string, remote_ip string, remote_port uint) (int, NSaaSNetFlow) {
	// Initialize the flow
	var flow_ptr *C.NSaaSNetFlow_t = C.__nsaas_init_flow()

	local_ip_int := ipv4_str_to_uint32(local_ip)
	remote_ip_int := ipv4_str_to_uint32(remote_ip)

	ret := C.__nsaas_connect_go((*C.NSaaSChannelCtx_t)(ctx), (C.uint)(local_ip_int), (C.uint)(remote_ip_int), C.ushort(remote_port), flow_ptr)
	return (int)(ret), convert_net_flow_go(flow_ptr)
}

// Listen on the local host and port.
func Listen(ctx *NSaaSChannelCtx, local_ip string, local_port uint) int {
	local_ip_int := ipv4_str_to_uint32(local_ip)
	ret := C.__nsaas_listen_go((*C.NSaaSChannelCtx_t)(ctx), (C.uint)(local_ip_int), C.ushort(local_port))
	return (int)(ret)
}

// Send message on the flow.
// NOTE: Currently, only one iov is supported.
func SendMsg(ctx *NSaaSChannelCtx, flow NSaaSNetFlow, base *uint8, iov_len uint) int {
	var iov C.NSaaSIovec_t
	iov.base = unsafe.Pointer(base)
	iov.len = C.size_t(iov_len)

	ret := C.__nsaas_sendmsg_go((*C.NSaaSChannelCtx_t)(ctx), iov, 1, convert_net_flow_c(flow))
	return (int)(ret)
}

// Receive message on the channel.
// NOTE: Currently, only one iov is supported.
func RecvMsg(ctx *NSaaSChannelCtx, base *uint8, iov_len uint) (int, NSaaSNetFlow) {
	var iov C.NSaaSIovec_t
	iov.base = unsafe.Pointer(base)
	iov.len = C.size_t(iov_len)

	flow := C.__nsaas_recvmsg_go((*C.NSaaSChannelCtx_t)(ctx), iov, 1)
	if flow.dst_ip == 0 {
		return -1, convert_net_flow_go(&flow)
	} else {
		return 0, convert_net_flow_go(&flow)
	}
}
