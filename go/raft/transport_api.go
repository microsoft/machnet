package main

import (
	"bytes"
	"context"
	"encoding/gob"
	"errors"
	"fmt"
	"github.com/HdrHistogram/hdrhistogram-go"
	"io"
	"os"
	"runtime"
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
	AppendEntriesRequest uint8 = iota
	RequestVoteRequest
	TimeoutNowRequest
	InstallSnapshotRequestStart
	InstallSnapshotRequestBuffer
	InstallSnapshotRequestClose
	AppendEntriesPipelineStart
	AppendEntriesPipelineSend
	AppendEntriesPipelineRecv
	AppendEntriesPipelineClose
	AppendEntriesRequestResponse
	RequestVoteRequestResponse
	TimeoutNowRequestResponse
	InstallSnapshotRequestStartResponse
	InstallSnapshotRequestBufferResponse
	InstallSnapshotRequestCloseResponse
	AppendEntriesPipelineStartResponse
	AppendEntriesPipelineSendResponse
	AppendEntriesPipelineRecvResponse
	AppendEntriesPipelineCloseResponse
	DummyResponse
)

const maxMessageLength = 4 * 1024

type TransportApi struct {
	localIp    raft.ServerAddress
	configJson string
	raftPort   int
	hostname   string

	sendChannelCtx    *machnet.MachnetChannelCtx
	receiveChannelCtx *machnet.MachnetChannelCtx

	rpcChan          chan raft.RPC
	heartbeatFunc    func(raft.RPC)
	heartbeatFuncMtx sync.Mutex
	heartbeatTimeout time.Duration

	flowsMtx         sync.Mutex
	flows            map[raft.ServerID]*flow
	rpcId            uint64
	mu               sync.Mutex
	histogram        *hdrhistogram.Histogram
	lastRecordedTime time.Time
}

type RpcMessage struct {
	MsgType uint8
	RpcId   uint64
	Payload []byte
}

type raftPipelineAPI struct {
	t   *TransportApi
	id  raft.ServerID
	ctx context.Context

	cancel           func()
	inflightChMtx    sync.Mutex
	inflightCh       chan *AFuture
	doneCh           chan raft.AppendFuture
	histogram        *hdrhistogram.Histogram
	lastRecordedTime time.Time
}

type AFuture struct {
	raft.AppendFuture
	start    time.Time
	request  *raft.AppendEntriesRequest
	response *raft.AppendEntriesResponse
	err      error
	done     chan struct{}
}

func NewTransport(localIp raft.ServerAddress, sendChannelCtx *machnet.MachnetChannelCtx, receiveChannelCtx *machnet.MachnetChannelCtx, raftPort int, hostname string) *TransportApi {
	var trans = new(TransportApi)

	trans.localIp = localIp
	trans.sendChannelCtx = sendChannelCtx
	trans.receiveChannelCtx = receiveChannelCtx
	trans.configJson = "../servers.json"
	trans.raftPort = raftPort
	trans.flows = make(map[raft.ServerID]*flow)
	trans.rpcChan = make(chan raft.RPC, 100)
	trans.hostname = hostname
	trans.rpcId = 0
	trans.histogram = hdrhistogram.New(1, 100000000, 3)
	trans.lastRecordedTime = time.Now()

	return trans
}

// Consumer Interface function that returns a channel that can be used to consume and respond to RPC requests.
func (t *TransportApi) Consumer() <-chan raft.RPC {
	return t.rpcChan
}

// LocalAddr Interface function that is used to return our local address to distinguish from our peers.
func (t *TransportApi) LocalAddr() raft.ServerAddress {
	return t.localIp
}

func (t *TransportApi) BootstrapPeers(numPeers int) {
	t.flowsMtx.Lock()
	for i := 0; i < numPeers; i++ {
		id := fmt.Sprintf("node%d", i)

		if id == t.hostname {
			continue
		}

		jsonBytes, err := os.ReadFile(t.configJson)
		if err != nil {
			glog.Fatal("Failed to read config file")
		}

		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", id, "ipv4_addr")
		glog.Infof("%s connecting to %s", t.localIp, remoteIp)

		ret, f := machnet.Connect(t.sendChannelCtx, string(t.localIp), remoteIp, uint(t.raftPort))
		if ret != 0 {
			glog.Fatalf("Failed to connect to remote host: %s->%s", t.localIp, remoteIp)
		}
		t.flows[raft.ServerID(id)] = &f
	}
	t.flowsMtx.Unlock()
}

func (t *TransportApi) getPeer(id raft.ServerID) (flow, error) {
	t.flowsMtx.Lock()
	_, ok := t.flows[id]
	if !ok {

		jsonBytes, err := os.ReadFile(t.configJson)
		if err != nil {
			glog.Fatal("Failed to read config file")
		}

		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", string(id), "ipv4_addr")

		glog.Info("Trying to connect to ", remoteIp, ":", t.raftPort, " from ", t.localIp)
		ret, f := machnet.Connect(t.sendChannelCtx, string(t.localIp), remoteIp, uint(t.raftPort))
		if ret != 0 {
			glog.Fatal("Failed to connect to remote host")
		}
		t.flows[id] = &f
	}
	f := t.flows[id]
	t.flowsMtx.Unlock()
	return *f, nil
}

// SendMachnetRpc Generic Machnet RPC handler.
// Encodes the given payload and rpcType into a rpcMessage and sends it to the remote host using Machnet library functions.
func (t *TransportApi) SendMachnetRpc(id raft.ServerID, rpcType uint8, payload []byte) (resp RpcMessage, err error) {

	t.mu.Lock()
	defer t.mu.Unlock()
	flow, err := t.getPeer(id)
	if err != nil {
		return RpcMessage{}, err
	}

	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	var rpcId = t.rpcId
	msg := RpcMessage{MsgType: rpcType, RpcId: rpcId, Payload: payload}
	t.rpcId = 1 + t.rpcId
	if err := enc.Encode(msg); err != nil {
		return RpcMessage{}, err
	}

	msgBytes := buff.Bytes()
	msgLen := len(msgBytes)
	start := time.Now()
	//glog.Infof("Called machnet.SendMsg at %v", start)
	ret := machnet.SendMsg(t.sendChannelCtx, flow, &msgBytes[0], uint(msgLen))
	if ret != 0 {
		return RpcMessage{}, errors.New("failed to send message to remote host")
	}

	responseBuff := make([]byte, maxMessageLength)

	recvBytes := 0
	for recvBytes == 0 {
		recvBytes, _ = machnet.Recv(t.sendChannelCtx, &responseBuff[0], maxMessageLength)
		if recvBytes < 0 {
			glog.Error("Failed to receive response from remote host")
			return RpcMessage{}, errors.New("failed to receive response from remote host")
		}
		runtime.Gosched()
	}

	//glog.Infof("Received response at %v, took : %v", time.Now(), time.Since(start))
	err = t.histogram.RecordValue(time.Since(start).Microseconds())
	if err != nil {
		glog.Error("Failed to record to histogram")
	}
	if time.Since(t.lastRecordedTime) > 1*time.Second {
		percentileValues := t.histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
		glog.Warningf("RPC processing time: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us RPS: %d",
			float64(percentileValues[50.0]), float64(percentileValues[95.0]),
			float64(percentileValues[99.0]), float64(percentileValues[99.9]), t.histogram.TotalCount())
		t.histogram.Reset()
		t.lastRecordedTime = time.Now()
	}

	buff.Reset()
	if n, _ := buff.Write(responseBuff[:recvBytes]); n != recvBytes {
		return RpcMessage{}, errors.New("failed to write response into buffer")
	}

	var response RpcMessage
	if err := dec.Decode(&response); err != nil {
		glog.Error("Failed to decode response from remote host")
		return RpcMessage{}, err
	}

	return response, nil
}

// AppendEntries sends the appropriate RPC to the target node.
func (t *TransportApi) AppendEntries(id raft.ServerID, target raft.ServerAddress, args *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	rpcResponse, err := t.SendMachnetRpc(id, AppendEntriesRequest, reqBytes)
	payloadBytes := rpcResponse.Payload

	if err != nil {
		glog.Errorf("AppendEntries: failed to SendMachnetRPC")
		return err
	}

	buff.Reset()
	if n, _ := buff.Write(payloadBytes); n != len(payloadBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		glog.Errorf("AppendEntries: failed to decode: %v ", rpcResponse)
		return err
	}
	return nil
}

// RequestVote sends the appropriate RPC to the target node.
func (t *TransportApi) RequestVote(id raft.ServerID, target raft.ServerAddress, args *raft.RequestVoteRequest, resp *raft.RequestVoteResponse) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	rpcResponse, err := t.SendMachnetRpc(id, RequestVoteRequest, reqBytes)
	recvBytes := rpcResponse.Payload
	if err != nil {
		return err
	}

	buff.Reset()
	if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		return err
	}

	return nil
}

// TimeoutNow is used to start a leadership transfer to the target node.
func (t *TransportApi) TimeoutNow(id raft.ServerID, target raft.ServerAddress, args *raft.TimeoutNowRequest, resp *raft.TimeoutNowResponse) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	rpcResponse, err := t.SendMachnetRpc(id, TimeoutNowRequest, reqBytes)
	recvBytes := rpcResponse.Payload
	if err != nil {
		return err
	}

	buff.Reset()
	if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		return err
	}

	return nil
}

// InstallSnapshot is used to push a snapshot down to a follower. The data is read from
// the ReadCloser and streamed to the client.
func (t *TransportApi) InstallSnapshot(id raft.ServerID, target raft.ServerAddress, req *raft.InstallSnapshotRequest, resp *raft.InstallSnapshotResponse, data io.Reader) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	if err := enc.Encode(req); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	rpcResponse, err := t.SendMachnetRpc(id, InstallSnapshotRequestStart, reqBytes)
	if err != nil {
		return err
	}

	var buf [16384]byte
	for {
		n, err := data.Read(buf[:])
		if err == io.EOF || (err == nil && n == 0) {
			break
		}
		if err != nil {
			return err
		}

		rpcResponse, err = t.SendMachnetRpc(id, InstallSnapshotRequestBuffer, buf[:n])
		if err != nil {
			return err
		}
	}

	dummyPayload := make([]byte, 1)
	rpcResponse, err = t.SendMachnetRpc(id, InstallSnapshotRequestClose, dummyPayload)
	recvBytes := rpcResponse.Payload
	if err != nil {
		return err
	}

	buff.Reset()
	if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		return err
	}

	return nil
}

// SetHeartbeatHandler is used to set up a heartbeat handler
// as a fast-pass. This is to avoid head-of-line blocking from
// disk IO. If Transport does not support this, it can simply
// ignore the call, and push the heartbeat onto the Consumer channel.
func (t *TransportApi) SetHeartbeatHandler(cb func(rpc raft.RPC)) {
	t.heartbeatFuncMtx.Lock()
	t.heartbeatFunc = cb
	t.heartbeatFuncMtx.Unlock()
}

// AppendEntriesPipeline returns an interface that can be used to pipeline
// AppendEntries requests.
func (t *TransportApi) AppendEntriesPipeline(id raft.ServerID, target raft.ServerAddress) (raft.AppendPipeline, error) {
	ctx := context.TODO()
	_, cancel := context.WithCancel(ctx)

	// Send Machnet RPC to remote host. Send a dummy payload.
	// We don't care about the response as this is just to register the AppendEntriesPipeline request.
	dummyPayload := make([]byte, 1)
	_, err := t.SendMachnetRpc(id, AppendEntriesPipelineStart, dummyPayload)
	if err != nil {
		glog.Errorf("AppendEntriesPipeline not functioning properly")
		cancel()
		return nil, err
	}

	pipelineObject := raftPipelineAPI{
		t:                t,
		id:               id,
		ctx:              ctx,
		cancel:           cancel,
		inflightCh:       make(chan *AFuture),
		doneCh:           make(chan raft.AppendFuture),
		histogram:        hdrhistogram.New(1, 100000000, 3),
		lastRecordedTime: time.Now(),
	}
	go pipelineObject.receiver()
	return &pipelineObject, nil
}

// AppendEntries is used to add another request to the pipeline.
// The send may block which is an effective form of back-pressure.
func (r *raftPipelineAPI) AppendEntries(req *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) (raft.AppendFuture, error) {
	af := &AFuture{
		start:    time.Now(),
		request:  req,
		response: resp,
		done:     make(chan struct{}),
	}

	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)

	if err := enc.Encode(req); err != nil {
		return nil, err
	}
	reqBytes := buff.Bytes()
	_, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineSend, reqBytes)
	if err != nil {
		return nil, err
	}

	r.inflightChMtx.Lock()
	select {
	case <-r.ctx.Done():
	default:
		glog.Warningf("AEPipelineSend at %v", time.Now())
		r.inflightCh <- af
	}
	r.inflightChMtx.Unlock()
	return af, nil
}

// Consumer returns a channel that can be used to consume
// response futures when they are ready.
func (r *raftPipelineAPI) Consumer() <-chan raft.AppendFuture {
	return r.doneCh
}

// Close closes the pipeline and cancels all inflight RPCs
func (r *raftPipelineAPI) Close() error {
	r.cancel()

	dummyPayload := make([]byte, 1)
	_, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineClose, dummyPayload)
	if err != nil {
		glog.Infof("Close: failed to send AppendEntriesPipelineClose")
		return err
	}

	r.inflightChMtx.Lock()
	close(r.inflightCh)
	r.inflightChMtx.Unlock()
	return nil
}

func (r *raftPipelineAPI) receiver() {
	start := time.Now()
	for af := range r.inflightCh {
		var buff bytes.Buffer
		dec := gob.NewDecoder(&buff)

		var resp raft.AppendEntriesResponse

		dummyPayload := make([]byte, 1)
		//start := time.Now()
		rpcResponse, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineRecv, dummyPayload)
		r.histogram.RecordValue(time.Since(start).Microseconds())
		recvBytes := rpcResponse.Payload
		if err == nil {
			buff.Reset()
			if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
				err = errors.New("failed to write payload into buffer")
			} else {
				err = dec.Decode(&resp)
			}
		}

		if err != nil {
			af.err = err
		} else {
			af.response.Term = resp.Term
			af.response.Success = resp.Success
			af.response.LastLog = resp.LastLog
		}

		close(af.done)
		//start := time.Now()
		r.doneCh <- af
		//r.histogram.RecordValue(time.Since(start).Microseconds())
		if time.Since(r.lastRecordedTime) > 1*time.Second {
			percentileValues := r.histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
			glog.Warningf("[ Receiver rpc time 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us]",
				float64(percentileValues[50.0])/1000, float64(percentileValues[95.0])/1000,
				float64(percentileValues[99.0])/1000, float64(percentileValues[99.9])/1000)
			r.histogram.Reset()
			r.lastRecordedTime = time.Now()
		}
	}
}

// Error blocks until the future arrives and then
// returns the error status of the future.
// This may be called any number of times - all
// calls will return the same value.
// Note that it is not OK to call this method
// twice concurrently on the same Future instance.
func (f *AFuture) Error() error {
	//start := time.Now()
	//return errors.New("dummy error")
	//glog.Warningf("Error: started to block at %+v", start)
	//pprof.Lookup("goroutine").WriteTo(os.Stderr, 1)
	<-f.done
	//glog.Warningf("Error: finished blocking took %v", time.Since(start))
	return f.err
}

// Start returns the time that the append request was started.
// It is always OK to call this method.
func (f *AFuture) Start() time.Time {
	return f.start
}

// Request holds the parameters of the AppendEntries call.
// It is always OK to call this method.
func (f *AFuture) Request() *raft.AppendEntriesRequest {
	return f.request
}

// Response holds the results of the AppendEntries call.
// This method must only be called after the Error
// method returns, and will only be valid on success.
func (f *AFuture) Response() *raft.AppendEntriesResponse {
	return f.response
}

// EncodePeer is used to serialize a peer's address.
func (t *TransportApi) EncodePeer(id raft.ServerID, addr raft.ServerAddress) []byte {
	return []byte(addr)
}

// DecodePeer is used to deserialize a peer's address.
func (t *TransportApi) DecodePeer(p []byte) raft.ServerAddress {
	return raft.ServerAddress(p)
}
