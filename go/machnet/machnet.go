package machnet

// #cgo LDFLAGS: -L${SRCDIR}/../../build/release_build/src/core -lcore -L${SRCDIR}/../../build/release_build/src/ext -lmachnet_shim -lrt -Wl,-rpath=${SRCDIR}/../../build/release_build/src/core:${SRCDIR}/../../build/release_build/src/ext -fsanitize=address
// #cgo CFLAGS: -I${SRCDIR}/../../src/ext -I${SRCDIR}/../../src/include -fsanitize=address
// #include <stdlib.h>
// #include "conversion.h"
// #include "../../src/ext/machnet.h"
// #include "../../src/ext/machnet_common.h"
import "C"
import (
	"strconv"
	"strings"
	"unsafe"
)

// Alternate Go Type Defs for C types
type MachnetChannelCtx = C.MachnetChannelCtx_t

// Define the MachnetFlow struct
type MachnetFlow struct {
	SrcIp   uint32
	DstIp   uint32
	SrcPort uint16
	DstPort uint16
}

// Helper function to convert a C.MachnetFlow_t to a MachnetFlow.
func convert_net_flow_go(c_flow *C.MachnetFlow_t) MachnetFlow {
	return MachnetFlow{
		SrcIp:   (uint32)(c_flow.src_ip),
		DstIp:   (uint32)(c_flow.dst_ip),
		SrcPort: (uint16)(c_flow.src_port),
		DstPort: (uint16)(c_flow.dst_port),
	}
}

// Helper function to convert a MachnetFlow to a C.MachnetFlow_t.
func convert_net_flow_c(flow MachnetFlow) C.MachnetFlow_t {
	return C.MachnetFlow_t{
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

// Initialize the Machnet shim.
func Init() int {
	ret := C.machnet_init()
	return (int)(ret)
}

// Attach to the Machnet shim.
// Returns a pointer to the channel context.
func Attach() *MachnetChannelCtx {
	var c_ctx *C.MachnetChannelCtx_t = (*C.MachnetChannelCtx_t)(C.machnet_attach())
	return (*MachnetChannelCtx)(c_ctx)
}

// Connect to the remote host and port.
func Connect(ctx *MachnetChannelCtx, local_ip string, remote_ip string, remote_port uint) (int, MachnetFlow) {
	// Initialize the flow
	var flow_ptr *C.MachnetFlow_t = C.__machnet_init_flow()

	local_ip_str := C.CString(local_ip)
	remote_ip_str := C.CString(remote_ip)

	ret := C.__machnet_connect_go((*C.MachnetChannelCtx_t)(ctx), local_ip_str, remote_ip_str, C.ushort(remote_port), flow_ptr)
	return (int)(ret), convert_net_flow_go(flow_ptr)
}

// Listen on the local host and port.
func Listen(ctx *MachnetChannelCtx, local_ip string, local_port uint) int {
	local_ip_str := C.CString(local_ip)
	ret := C.__machnet_listen_go((*C.MachnetChannelCtx_t)(ctx), local_ip_str, C.ushort(local_port))
	return (int)(ret)
}

// Send message on the flow.
// NOTE: Currently, only one iov is supported.
func SendMsg(ctx *MachnetChannelCtx, flow MachnetFlow, base *uint8, iov_len uint) int {
	//fmt.Printf("Entered machnet.SendMsg at %v\n", time.Now())
	var iov C.MachnetIovec_t
	iov.base = unsafe.Pointer(base)
	iov.len = C.size_t(iov_len)

	ret := C.__machnet_sendmsg_go((*C.MachnetChannelCtx_t)(ctx), iov, 1, convert_net_flow_c(flow))
	return (int)(ret)
}

// Receive bytes on the channel.
// Returns the number of bytes received and the flow. If error occurs, number of bytes received is -1.
func Recv(ctx *MachnetChannelCtx, base *uint8, len uint, blocking bool) (int, MachnetFlow) {

	var recv_len int
	flow := C.__machnet_recv_go((*C.MachnetChannelCtx_t)(ctx), unsafe.Pointer(base), C.size_t(len), (*C.long)(unsafe.Pointer(&recv_len)), C.size_t(blocking))
	return recv_len, convert_net_flow_go(&flow)
}

// Receive message on the channel.
// NOTE: Currently, only one iov is supported.
func RecvMsg(ctx *MachnetChannelCtx, base *uint8, iov_len uint, blocking bool) (int, MachnetFlow) {
	var iov C.MachnetIovec_t
	iov.base = unsafe.Pointer(base)
	iov.len = C.size_t(iov_len)

	flow := C.__machnet_recvmsg_go((*C.MachnetChannelCtx_t)(ctx), iov, 1, C.size_t(blocking))
	if flow.dst_ip == 0 {
		return -1, convert_net_flow_go(&flow)
	} else {
		return 0, convert_net_flow_go(&flow)
	}
}
