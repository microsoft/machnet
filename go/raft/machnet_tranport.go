package main

import (
	"bytes"
	"encoding/gob"
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"time"

	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"github.com/microsoft/machnet"
)

type flow = machnet.MachnetFlow

// Enum for Message types
const (
	rpcAppendEntries uint8 = iota
	rpcRequestVote
	rpcInstallSnapshot
	rpcTimeoutNow

	// rpcMaxPipeline controls the maximum number of outstanding
	// AppendEntries RPC calls.
	rpcMaxPipeline = 128
)

const maxMessageLength = 4 * 1024

type MachnetTransport struct {
	localIp    raft.ServerAddress
	configJson string
	raftPort   int
	hostname   string

	channelCtx *machnet.MachnetChannelCtx

	consumeCh chan raft.RPC

	heartbeatFn     func(raft.RPC)
	heartbeatFnLock sync.Mutex

	//heartbeatTimeout time.Duration
	shutdown     bool
	shutdownCh   chan struct{}
	shutdownLock sync.Mutex

	flowsLock sync.Mutex
	flows     map[raft.ServerID]*flow
}

//type RpcMessage struct {
//	MsgType uint8
//	RpcId   uint64
//	Payload []byte
//}

type machnetPipeline struct {
	mt           *MachnetTransport
	f            *flow
	doneCh       chan raft.AppendFuture
	inProgressCh chan *appendFuture

	shutdown     bool
	shutdownCh   chan struct{}
	shutdownLock sync.Mutex
}

// used to wait on pipelined append entries RPC
type appendFuture struct {
	raft.AppendFuture

	start    time.Time
	request  *raft.AppendEntriesRequest
	response *raft.AppendEntriesResponse
	err      error
	done     chan struct{}
}

func NewMachnetTransport(localIp raft.ServerAddress, channelCtx *machnet.MachnetChannelCtx, raftPort int, hostname string) *MachnetTransport {
	return &MachnetTransport{
		localIp:    localIp,
		configJson: "./servers.json",
		raftPort:   raftPort,
		hostname:   hostname,
		shutdownCh: make(chan struct{}),
		channelCtx: channelCtx,
		consumeCh:  make(chan raft.RPC),
		flows:      make(map[raft.ServerID]*flow),
	}
}

// SetHeartbeatHandler is used to set up a heartbeat handler
// as a fast-pass. This is to avoid head-of-line blocking from
// disk IO. If Transport does not support this, it can simply
// ignore the call, and push the heartbeat onto the Consumer channel.
func (mt *MachnetTransport) SetHeartbeatHandler(cb func(rpc raft.RPC)) {
	mt.heartbeatFnLock.Lock()
	mt.heartbeatFn = cb
	mt.heartbeatFnLock.Unlock()
}

// Close is used to stop the Machnet transport.
func (mt *MachnetTransport) Close() error {
	mt.shutdownLock.Lock()
	defer mt.shutdownLock.Unlock()

	if !mt.shutdown {
		close(mt.shutdownCh)
		mt.shutdown = true
	}
	return nil
}

// Consumer Interface function that returns a channel that can be used to consume and respond to RPC requests.
func (mt *MachnetTransport) Consumer() <-chan raft.RPC {
	return mt.consumeCh
}

// LocalAddr Interface function that is used to return our local address to distinguish from our peers.
func (mt *MachnetTransport) LocalAddr() raft.ServerAddress {
	return mt.localIp
}

// AppendEntriesPipeline returns an interface that can be used to pipeline
// AppendEntries requests.
func (mt *MachnetTransport) AppendEntriesPipeline(id raft.ServerID, target raft.ServerAddress) (raft.AppendPipeline, error) {
	mt.flowsLock.Lock()
	defer mt.flowsLock.Unlock()
	// TODO: check not ok if any problem occur
	f := mt.flows[id]
	return newMachnetPipeline(mt, f), nil
}

// AppendEntries implements the Transport interface.
func (mt *MachnetTransport) AppendEntries(id raft.ServerID, target raft.ServerAddress, args *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) error {
	return mt.genericRPC(id, target, rpcAppendEntries, args, resp)
}

// RequestVote implements the Transport interface.
func (mt *MachnetTransport) RequestVote(id raft.ServerID, target raft.ServerAddress, args *raft.RequestVoteRequest, resp *raft.RequestVoteResponse) error {
	return mt.genericRPC(id, target, rpcRequestVote, args, resp)
}

// genericRPC handles a simple request/response RPC
func (mt *MachnetTransport) genericRPC(id raft.ServerID, target raft.ServerAddress, rpcType uint8, args interface{}, resp interface{}) error {
	f, err := mt.getPeer(id)
	if err != nil {
		return err
	}

	// send rpc request
	// first send a byte of MsgType
	if ret := machnet.SendMsg(mt.channelCtx, f, &rpcType, 1); ret != 0 {
		return errors.New("failed to send machnet rpc (msgType byte)")
	}

	var buf bytes.Buffer
	enc := gob.NewEncoder(&buf)
	dec := gob.NewDecoder(&buf)
	if err := enc.Encode(args); err != nil {
		return err
	}
	msgBytes := buf.Bytes()
	if ret := machnet.SendMsg(mt.channelCtx, f, &msgBytes[0], uint(len(msgBytes))); ret != 0 {
		return errors.New("failed to send machnet rpc (args)")
	}
	// receive rpc response
	respBuf := make([]byte, maxMessageLength)
	recvBytes := 0
	for recvBytes == 0 {
		if recvBytes, _ = machnet.Recv(mt.channelCtx, &respBuf[0], maxMessageLength); recvBytes < 0 {
			return errors.New("failed to receive rpc response")
		}
	}

	buf.Reset()
	if n, _ := buf.Write(respBuf[:recvBytes]); n != recvBytes {
		return errors.New("failed to write response to buffer")
	}
	if err := dec.Decode(resp); err != nil {
		return errors.New("failed to decode to resp")
	}
	return nil
}

func (mt *MachnetTransport) InstallSnapshot(id raft.ServerID, target raft.ServerAddress, args *raft.InstallSnapshotRequest, resp *raft.InstallSnapshotResponse, data io.Reader) error {
	f, err := mt.getPeer(id)
	if err != nil {
		return err
	}
	rpcType := rpcInstallSnapshot

	// send rpc request
	// first send a byte of MsgType
	if ret := machnet.SendMsg(mt.channelCtx, f, &rpcType, 1); ret != 0 {
		return errors.New("failed to send machnet rpc (msgType byte)")
	}
	var buf bytes.Buffer
	enc := gob.NewEncoder(&buf)
	dec := gob.NewDecoder(&buf)
	if err := enc.Encode(args); err != nil {
		return err
	}
	msgBytes := buf.Bytes()
	if ret := machnet.SendMsg(mt.channelCtx, f, &msgBytes[0], uint(len(msgBytes))); ret != 0 {
		return errors.New("failed to send machnet rpc (args)")
	}
	var tmpBuf [16384]byte
	for {
		n, err := data.Read(tmpBuf[:])
		if err == io.EOF || (err == nil && n == 0) {
			break
		}
		if err != nil {
			return err
		}
		if ret := machnet.SendMsg(mt.channelCtx, f, &tmpBuf[:n][0], uint(n)); ret != 0 {
			return errors.New("failed to send machnet rpc (copy snapshot)")
		}
	}
	// receive rpc response
	respBuf := make([]byte, maxMessageLength)
	recvBytes := 0
	for recvBytes == 0 {
		if recvBytes, _ = machnet.Recv(mt.channelCtx, &respBuf[0], maxMessageLength); recvBytes < 0 {
			return errors.New("failed to receive rpc response")
		}
	}

	buf.Reset()
	if n, _ := buf.Write(respBuf[:recvBytes]); n != recvBytes {
		return errors.New("failed to write response to buffer")
	}
	if err := dec.Decode(resp); err != nil {
		return errors.New("failed to decode to resp")
	}
	return nil
}

// EncodePeer implements the Transport interface.
func (mt *MachnetTransport) EncodePeer(id raft.ServerID, addr raft.ServerAddress) []byte {
	return []byte(addr)
}

// DecodePeer implements the Transport interface.
func (mt *MachnetTransport) DecodePeer(buf []byte) raft.ServerAddress {
	return raft.ServerAddress(buf)
}

// TimeoutNow implements the Transport interface.
func (mt *MachnetTransport) TimeoutNow(id raft.ServerID, target raft.ServerAddress, args *raft.TimeoutNowRequest, resp *raft.TimeoutNowResponse) error {
	return mt.genericRPC(id, target, rpcTimeoutNow, args, resp)
}

func newMachnetPipeline(mt *MachnetTransport, f *flow) *machnetPipeline {
	mp := &machnetPipeline{
		mt:           mt,
		f:            f,
		doneCh:       make(chan raft.AppendFuture, rpcMaxPipeline),
		inProgressCh: make(chan *appendFuture, rpcMaxPipeline),
		shutdownCh:   make(chan struct{}),
	}
	go mp.decodeResponses()
	return mp
}

// decodeResponses is a long-running routine that decodes the responses
// sent on the pipeline
func (mp *machnetPipeline) decodeResponses() {
	for {
		select {
		case future := <-mp.inProgressCh:

			respBuf := make([]byte, maxMessageLength)
			var err error
			var buf bytes.Buffer
			dec := gob.NewDecoder(&buf)

			recvBytes := 0
			for recvBytes == 0 {
				if recvBytes, _ = machnet.Recv(mp.mt.channelCtx, &respBuf[0], maxMessageLength); recvBytes < 0 {
					err = errors.New("failed to receive rpc response")
				}
			}

			buf.Reset()
			if n, _ := buf.Write(respBuf[:recvBytes]); n != recvBytes {
				err = errors.New("failed to write response to buffer")
			}
			err = dec.Decode(future.Response())
			future.err = err
			close(future.done)
			select {
			case mp.doneCh <- future:
			case <-mp.shutdownCh:
				return

			}
		case <-mp.shutdownCh:
			return

		}
	}
}

func (mp *machnetPipeline) AppendEntries(args *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) (raft.AppendFuture, error) {
	future := &appendFuture{
		start:    time.Now(),
		request:  args,
		response: resp,
		done:     make(chan struct{}, 1),
	}

	// send
	rpcType := rpcAppendEntries
	if ret := machnet.SendMsg(mp.mt.channelCtx, *mp.f, &rpcType, 1); ret != 0 {
		return nil, errors.New("failed to send machnet rpc (msgType byte)")
	}

	var buf bytes.Buffer
	enc := gob.NewEncoder(&buf)
	if err := enc.Encode(future.request); err != nil {
		return nil, err
	}
	msgBytes := buf.Bytes()
	if ret := machnet.SendMsg(mp.mt.channelCtx, *mp.f, &msgBytes[0], uint(len(msgBytes))); ret != 0 {
		return nil, errors.New("failed to send machnet rpc (args)")
	}

	select {
	case mp.inProgressCh <- future:
		return future, nil
	case <-mp.shutdownCh:
		return nil, raft.ErrPipelineShutdown
	}
}

func (mp *machnetPipeline) Consumer() <-chan raft.AppendFuture {
	return mp.doneCh
}

func (mp *machnetPipeline) Close() error {
	mp.shutdownLock.Lock()
	defer mp.shutdownLock.Unlock()
	if mp.shutdown {
		return nil
	}
	mp.shutdown = true
	close(mp.shutdownCh)
	return nil
}

// TODO: adjust usage
func (mt *MachnetTransport) BootstrapPeers(numPeers int) {

	mt.flowsLock.Lock()
	defer mt.flowsLock.Unlock()

	for i := 0; i < numPeers; i++ {
		id := fmt.Sprintf("node%d", i)

		if id == mt.hostname {
			continue
		}

		jsonBytes, err := os.ReadFile(mt.configJson)
		if err != nil {
			glog.Fatal("Failed to read config file")
		}

		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", id, "ipv4_addr")
		glog.Infof("%s connecting to %s", mt.localIp, remoteIp)

		ret, f := machnet.Connect(mt.channelCtx, string(mt.localIp), remoteIp, uint(mt.raftPort))
		if ret != 0 {
			glog.Fatalf("Failed to connect to remote host: %s->%s", mt.localIp, remoteIp)
		}
		mt.flows[raft.ServerID(id)] = &f
	}
}

// TODO: may use
func (mt *MachnetTransport) getPeer(id raft.ServerID) (flow, error) {
	mt.flowsLock.Lock()
	_, ok := mt.flows[id]
	if !ok {
		// Read the contents of file config_json into a byte array.
		jsonBytes, err := os.ReadFile(mt.configJson)
		if err != nil {
			glog.Fatal("Failed to read config file")
		}

		// Parse the json file to get the remote_ip.
		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", string(id), "ipv4_addr")

		// Initiate connection to the remote host.
		glog.Info("Trying to connect to ", remoteIp, ":", mt.raftPort, " from ", mt.localIp)
		ret, f := machnet.Connect(mt.channelCtx, string(mt.localIp), remoteIp, uint(mt.raftPort))
		if ret != 0 {
			glog.Fatal("Failed to connect to remote host")
		}
		mt.flows[id] = &f
	}
	f := mt.flows[id]
	mt.flowsLock.Unlock()
	return *f, nil
}

// Error blocks until the future arrives and then
// returns the error status of the future.
// This may be called any number of times - all
// calls will return the same value.
// Note that it is not OK to call this method
// twice concurrently on the same Future instance.
func (f *appendFuture) Error() error {
	<-f.done
	return f.err
}

// Start returns the time that the append request was started.
// It is always OK to call this method.
func (f *appendFuture) Start() time.Time {
	return f.start
}

// Request holds the parameters of the AppendEntries call.
// It is always OK to call this method.
func (f *appendFuture) Request() *raft.AppendEntriesRequest {
	return f.request
}

// Response holds the results of the AppendEntries call.
// This method must only be called after the Error
// method returns, and will only be valid on success.
func (f *appendFuture) Response() *raft.AppendEntriesResponse {
	return f.response
}
