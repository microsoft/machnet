package main

import (
	"bytes"
	"context"
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
	Response
)

const maxMessageLength = 4 * 1024

type TransportApi struct {
	localIp    raft.ServerAddress
	configJson string
	raftPort   int
	hostname   string

	numMessages int

	sendChannelCtx    *machnet.MachnetChannelCtx
	receiveChannelCtx *machnet.MachnetChannelCtx

	rpcChan          chan raft.RPC
	heartbeatFunc    func(raft.RPC)
	heartbeatFuncMtx sync.Mutex
	heartbeatTimeout time.Duration

	flowsMtx sync.Mutex
	flows    map[raft.ServerID]*flow
}

type rpcMessage struct {
	MsgType uint8
	Payload []byte
}

type raftPipelineAPI struct {
	t   *TransportApi
	id  raft.ServerID
	ctx context.Context

	cancel        func()
	inflightChMtx sync.Mutex
	inflightCh    chan *appendFuture
	doneCh        chan raft.AppendFuture
}

type appendFuture struct {
	raft.AppendFuture

	start    time.Time
	request  *raft.AppendEntriesRequest
	response *raft.AppendEntriesResponse
	err      error
	done     chan struct{}
}

func NewTransport(localIp raft.ServerAddress, sendChannelCtx *machnet.MachnetChannelCtx, receiveChannelCtx *machnet.MachnetChannelCtx, raftPort int, hostname string) *TransportApi {
	var trans *TransportApi = new(TransportApi)

	trans.localIp = localIp
	trans.sendChannelCtx = sendChannelCtx
	trans.receiveChannelCtx = receiveChannelCtx
	trans.configJson = "../servers.json"
	trans.raftPort = raftPort
	trans.flows = make(map[raft.ServerID]*flow)
	trans.rpcChan = make(chan raft.RPC, 100)
	trans.hostname = hostname

	return trans
}

// Interface function that returns a channel that can be used to consume and respond to RPC requests.
func (t *TransportApi) Consumer() <-chan raft.RPC {
	return t.rpcChan
}

// Interface function that is used to return our local address to distinguish from our peers.
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

		// Parse the json file to get the remote_ip.
		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", string(id), "ipv4_addr")

		// Initiate connection to the remote host.
		ret, f := machnet.Connect(t.sendChannelCtx, string(t.localIp), remoteIp, uint(t.raftPort))
		if ret != 0 {
			glog.Fatal("Failed to connect to remote host")
		}
		t.flows[raft.ServerID(id)] = &f
	}
	t.flowsMtx.Unlock()
}

func (t *TransportApi) getPeer(id raft.ServerID) (flow, error) {
	t.flowsMtx.Lock()
	_, ok := t.flows[id]
	if !ok {
		// Read the contents of file config_json into a byte array.
		jsonBytes, err := os.ReadFile(t.configJson)
		if err != nil {
			glog.Fatal("Failed to read config file")
		}

		// Parse the json file to get the remote_ip.
		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", string(id), "ipv4_addr")

		// Initiate connection to the remote host.
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
func (t *TransportApi) SendMachnetRpc(id raft.ServerID, rpcType uint8, payload []byte) (resp []byte, err error) {
	// Get the flow to the remote host.
	flow, err := t.getPeer(id)
	if err != nil {
		return nil, err
	}

	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	// Enclose the payload into a rpcMessage and then encode into byte array.
	msg := rpcMessage{MsgType: rpcType, Payload: payload}
	if err := enc.Encode(msg); err != nil {
		return nil, err
	}

	msgBytes := buff.Bytes()
	msgLen := len(msgBytes)

	t.numMessages += 1
	glog.V(5).Info("Sending message type ", rpcType, " of size: ", msgLen, " numSent: ", t.numMessages)

	// Send to the remote host on the flow.
	ret := machnet.SendMsg(t.sendChannelCtx, flow, &msgBytes[0], uint(msgLen))
	if ret != 0 {
		return nil, errors.New("failed to send message to remote host")
	}

	glog.V(5).Info("Message sent, waiting for response.")

	// Receive the response from the remote host on the flow.
	responseBuff := make([]byte, maxMessageLength)

	// Run until we receive a response from the remote host.
	recvBytes := 0
	for recvBytes == 0 {
		recvBytes, _ = machnet.Recv(t.sendChannelCtx, &responseBuff[0], maxMessageLength)
		if recvBytes < 0 {
			glog.Error("Failed to receive response from remote host")
			return nil, errors.New("failed to receive response from remote host")
		}
	}

	if rpcType == AppendEntriesPipelineRecv {
		glog.V(3).Info("Received AEPRecv response: ", responseBuff[:recvBytes])
	}

	// Get the rpcMessage from the byte array by writing into buffer and then decoding.
	buff.Reset()
	if n, _ := buff.Write(responseBuff[:recvBytes]); n != recvBytes {
		return nil, errors.New("failed to write response into buffer")
	}

	var response rpcMessage
	if err := dec.Decode(&response); err != nil {
		return nil, err
	}

	if rpcType == AppendEntriesRequest {
		glog.V(3).Info("Received RPC response of length: ", len(response.Payload))
	} else if rpcType == AppendEntriesPipelineRecv {
		glog.V(3).Info("Received AppendEntriesPipeline response: ", response.Payload)
	}
	glog.V(5).Info("Received RPC response from ", id, " of type ", response.MsgType, " length: ", len(response.Payload))

	// Return the payload of the response.
	return response.Payload, nil
}

// AppendEntries sends the appropriate RPC to the target node.
func (t *TransportApi) AppendEntries(id raft.ServerID, target raft.ServerAddress, args *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	// Encode the AppendEntriesRequest into a byte array.
	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	// Send NSaaS RPC to remote host.
	recvBytes, err := t.SendMachnetRpc(id, AppendEntriesRequest, reqBytes)
	if err != nil {
		return err
	}
	numRecvBytes := len(recvBytes)
	if numRecvBytes == 0 {
		glog.V(3).Info("Received empty response from ", id)
	}

	// Decode the AppendEntriesResponse from the received payload.
	buff.Reset()
	if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		return err
	}

	return nil
}

// RequestVote sends the appropriate RPC to the target node.
func (t *TransportApi) RequestVote(id raft.ServerID, target raft.ServerAddress, args *raft.RequestVoteRequest, resp *raft.RequestVoteResponse) error {
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)
	dec := gob.NewDecoder(&buff)

	// Encode the RequestVoteRequest into a byte array.
	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	// Send NSaaS RPC to remote host.
	recvBytes, err := t.SendMachnetRpc(id, RequestVoteRequest, reqBytes)
	if err != nil {
		return err
	}

	// Decode the RequestVoteResponse from the received payload.
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

	// Encode the TimeoutNowRequest into a byte array.
	if err := enc.Encode(args); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	// Send NSaaS RPC to remote host.
	recvBytes, err := t.SendMachnetRpc(id, TimeoutNowRequest, reqBytes)
	if err != nil {
		return err
	}

	// Decode the TimeoutNowResponse from the received payload.
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

	// Encode the InstallSnapshotRequest into a byte array.
	if err := enc.Encode(req); err != nil {
		return err
	}
	reqBytes := buff.Bytes()

	// Send NSaaS RPC to remote host. We don't care about the response as this is just to register the InstallSnapshot request.
	_, err := t.SendMachnetRpc(id, InstallSnapshotRequestStart, reqBytes)
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

		// Send NSaaS RPC to remote host.
		_, err = t.SendMachnetRpc(id, InstallSnapshotRequestBuffer, buf[:n])
		if err != nil {
			return err
		}
	}

	// Send NSaaS RPC to remote host to close the InstallSnapshot stream. Use a dummy payload.
	dummyPayload := make([]byte, 1)
	recvBytes, err := t.SendMachnetRpc(id, InstallSnapshotRequestClose, dummyPayload)
	if err != nil {
		return err
	}

	// Decode the InstallSnapshotResponse from the received payload.
	buff.Reset()
	if n, _ := buff.Write(recvBytes); n != len(recvBytes) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(resp); err != nil {
		return err
	}

	return nil
}

// SetHeartbeatHandler is used to setup a heartbeat handler
// as a fast-pass. This is to avoid head-of-line blocking from
// disk IO. If a Transport does not support this, it can simply
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

	// Send NSaaS RPC to remote host. Send a dummy payload.
	// We don't care about the response as this is just to register the AppendEntriesPipeline request.
	dummyPayload := make([]byte, 1)
	_, err := t.SendMachnetRpc(id, AppendEntriesPipelineStart, dummyPayload)
	if err != nil {
		cancel()
		return nil, err
	}

	pipelineObject := raftPipelineAPI{
		t:          t,
		id:         id,
		ctx:        ctx,
		cancel:     cancel,
		inflightCh: make(chan *appendFuture, 20),
		doneCh:     make(chan raft.AppendFuture, 20),
	}
	go pipelineObject.receiver()
	return &pipelineObject, nil
}

// AppendEntries is used to add another request to the pipeline.
// The send may block which is an effective form of back-pressure.
func (r *raftPipelineAPI) AppendEntries(req *raft.AppendEntriesRequest, resp *raft.AppendEntriesResponse) (raft.AppendFuture, error) {
	af := &appendFuture{
		start:    time.Now(),
		request:  req,
		response: resp,
		done:     make(chan struct{}),
	}

	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)

	// Encode the AppendEntriesRequest into a byte array.
	if err := enc.Encode(req); err != nil {
		return nil, err
	}
	reqBytes := buff.Bytes()

	// Send NSaaS RPC to remote host. The response will be saved as a Future object.
	_, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineSend, reqBytes)
	if err != nil {
		return nil, err
	}

	r.inflightChMtx.Lock()
	select {
	case <-r.ctx.Done():
	default:
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

	// Send NSaaS RPC to remote host. We don't care about the response as this is just to close AppendEntriesPipeline request.
	// Send a dummy payload.
	dummyPayload := make([]byte, 1)
	_, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineClose, dummyPayload)
	if err != nil {
		return err
	}

	r.inflightChMtx.Lock()
	close(r.inflightCh)
	r.inflightChMtx.Unlock()
	return nil
}

func (r *raftPipelineAPI) receiver() {
	for af := range r.inflightCh {
		var buff bytes.Buffer
		dec := gob.NewDecoder(&buff)

		var resp raft.AppendEntriesResponse

		// Send NSaaS RPC to remote host. Send a dummy payload.
		dummyPayload := make([]byte, 1)
		recvBytes, err := r.t.SendMachnetRpc(r.id, AppendEntriesPipelineRecv, dummyPayload)
		if err == nil {
			// Decode the AppendEntriesResponse from the received payload.
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
			// Copy resp to af.response.
			af.response.Term = resp.Term
			af.response.Success = resp.Success
			af.response.LastLog = resp.LastLog
			glog.V(3).Info("Received AppEntr response: ", resp)
		}
		close(af.done)
		r.doneCh <- af
	}
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

// EncodePeer is used to serialize a peer's address.
func (t *TransportApi) EncodePeer(id raft.ServerID, addr raft.ServerAddress) []byte {
	return []byte(addr)
}

// DecodePeer is used to deserialize a peer's address.
func (t *TransportApi) DecodePeer(p []byte) raft.ServerAddress {
	return raft.ServerAddress(p)
}
