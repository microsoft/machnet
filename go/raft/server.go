package main

import (
	"bytes"
	"errors"
	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/hashicorp/go-msgpack/codec"
	"io"
	"sync"
	"time"

	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"github.com/microsoft/machnet"
)

type PendingResponse struct {
	ch         chan raft.RPCResponse
	numPending int
	toClose    bool
}

type Server struct {
	transport *TransportApi

	// Assumption: There is only one snapshot per flow.
	snapshotReaders map[flow]*snapshotReader
	snapshotMutex   sync.Mutex

	// Assumption: There is only one pending snapshot response per flow.
	pendingSnapshotResponses map[flow]chan raft.RPCResponse

	// Assumption: There is only one pending pipeline channel per flow.
	pendingPipelineResponses map[flow]PendingResponse
	pipelineMutex            sync.Mutex
	histogram                *hdrhistogram.Histogram
	lastRecordedTime         time.Time
	msgCounts                map[uint8]int
}

type snapshotReader struct {
	buf      []byte
	bufMutex sync.Mutex
	numReady int // Number of bytes ready to be read
}

func (s *snapshotReader) Read(p []byte) (n int, err error) {
	if s.numReady == 0 {
		return 0, io.EOF
	}

	// Copy s.numReady bytes from s.buf to p
	s.bufMutex.Lock()
	n = copy(p, s.buf[:s.numReady])
	s.numReady -= n
	s.bufMutex.Unlock()

	if n != s.numReady {
		return 0, errors.New("failed to copy bytes from snapshotReader to p")
	}
	return n, nil
}

func (s *snapshotReader) Write(p []byte) (n int, err error) {
	s.bufMutex.Lock()
	n = copy(s.buf, p)
	s.numReady += n
	s.bufMutex.Unlock()

	if n != len(p) {
		return 0, errors.New("failed to copy bytes from p to snapshotReader")
	}
	return n, nil
}

func NewServer(transport *TransportApi) *Server {
	return &Server{
		transport:       transport,
		snapshotReaders: make(map[flow]*snapshotReader),

		pendingSnapshotResponses: make(map[flow]chan raft.RPCResponse),
		pendingPipelineResponses: make(map[flow]PendingResponse),
		histogram:                hdrhistogram.New(1, 1000000, 3),
		lastRecordedTime:         time.Now(),
		msgCounts:                make(map[uint8]int),
	}
}

func IsHeartbeat(command interface{}) bool {
	req, ok := command.(*raft.AppendEntriesRequest)
	if !ok {
		return false
	}
	return req.Term != 0 && req.PrevLogEntry == 0 && req.PrevLogTerm == 0 && len(req.Entries) == 0 && req.LeaderCommitIndex == 0
}

// StartServer Listens for incoming connection from leader, and start the Recv thread.
func (s *Server) StartServer() {
	// Listen for incoming connections.
	ret := machnet.Listen(s.transport.receiveChannelCtx, string(s.transport.localIp), uint(s.transport.raftPort))
	if ret != 0 {
		glog.Fatal("Failed to listen for incoming connections")
	}
	glog.Info("[LISTENING] [", s.transport.localIp, ":", s.transport.raftPort, "]")

	// Start listening for RPCs.
	s.HandleRPCs()
}

func DecodeRPCMessage(data []byte) (RpcMessage, error) {
	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	if n, _ := buff.Write(data); n != len(data) {
		return RpcMessage{}, errors.New("failed to write response into buffer")
	}

	var request RpcMessage
	if err := dec.Decode(&request); err != nil {
		return RpcMessage{}, err
	}
	return request, nil
}

// SendMachnetResponse sends a response to the Machnet Channel.
// The response is sent on the reverse flow.
//
// Parameters:
// response (rpcMessage): The response to be sent.
// flow (flow): The flow that received the original RPC. SendMachnetResponse will send the response on the reverse flow.
func (s *Server) SendMachnetResponse(response RpcMessage, flow flow, start time.Time) error {
	s.msgCounts[response.MsgType]++
	var buff bytes.Buffer
	//enc := gob.NewEncoder(&buff)
	enc := codec.NewEncoder(&buff, &codec.MsgpackHandle{})
	if err := enc.Encode(response); err != nil {
		glog.Errorf("SendMachnetResponse: failed to encode response: %v", err)
		return err
	}

	responseBytes := buff.Bytes()
	responseLen := len(responseBytes)

	tmpFlow := flow
	flow.SrcIp = tmpFlow.DstIp
	flow.DstIp = tmpFlow.SrcIp
	flow.SrcPort = tmpFlow.DstPort
	flow.DstPort = tmpFlow.SrcPort

	//elapsed := time.Since(start)
	//glog.Info("SendMachnetResponse: Total Rpc took: ", elapsed.Microseconds(), " us")

	ret := machnet.SendMsg(s.transport.receiveChannelCtx, flow, &responseBytes[0], uint(responseLen))

	if ret != 0 {
		return errors.New("SendMachnetResponse: failed to send message to remote host")
	}
	err := s.histogram.RecordValue(time.Since(start).Microseconds())
	if err != nil {
		glog.Errorf("Failed to record to histogram")
	}
	if time.Since(s.lastRecordedTime) > 1*time.Second {
		percentileValues := s.histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
		glog.Warningf("[RPC processing time: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us RPS: %d]",
			float64(percentileValues[50.0]), float64(percentileValues[95.0]),
			float64(percentileValues[99.0]), float64(percentileValues[99.9]), s.histogram.TotalCount())
		s.histogram.Reset()
		s.lastRecordedTime = time.Now()
		for msgType, count := range s.msgCounts {
			glog.Warningf("[M-%d: %d]", msgType, count)
			s.msgCounts[msgType] = 0
		}
	}
	return nil
}

func (s *Server) GetResponseFromChannel(ch <-chan raft.RPCResponse, flow flow, msgType uint8, rpcId uint64, start time.Time) error {
	//raftStart := time.Now()
	//glog.Info("GetResponseFromChannel: waiting for response from Raft channel...")
	resp := <-ch
	if resp.Error != nil {
		glog.Error("GetResponseFromChannel: couldn't handle Rpc; error: ", resp.Error)
		return resp.Error
	}

	var buff bytes.Buffer
	//enc := gob.NewEncoder(&buff)
	enc := codec.NewEncoder(&buff, &codec.MsgpackHandle{})
	switch msgType {
	case AppendEntriesRequest:
		payload := resp.Response.(*raft.AppendEntriesResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = AppendEntriesRequestResponse

	case RequestVoteRequest:
		payload := resp.Response.(*raft.RequestVoteResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = RequestVoteRequestResponse

	case TimeoutNowRequest:
		payload := resp.Response.(*raft.TimeoutNowResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = TimeoutNowRequestResponse
	case InstallSnapshotRequestStart:
		payload := resp.Response.(*raft.InstallSnapshotResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = InstallSnapshotRequestStartResponse
	case InstallSnapshotRequestClose:
		payload := resp.Response.(*raft.InstallSnapshotResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = InstallSnapshotRequestCloseResponse
	case AppendEntriesPipeline:
		payload := resp.Response.(*raft.AppendEntriesResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType = AppendEntriesPipelineResponse
	default:
		return errors.New("GetResponseFromChannel: Unknown message type")
	}

	// Construct the rpcMessage response.
	response := RpcMessage{
		MsgType: msgType,
		RpcId:   rpcId,
		Payload: buff.Bytes(),
	}

	if err := s.SendMachnetResponse(response, flow, start); err != nil {
		glog.Errorf("GetResponseFromChannel: failed to SendMachnetResponse: %v", err)
		return err
	}

	//raftElapsed := time.Since(raftStart)
	//glog.Info("GetResponseFromChannel: Raft Rpc took: ", raftElapsed.Microseconds(), " us")

	return nil
}

// HandleRaftCommand Send a Raft command to the Raft channel.
// To be used for all operations except AppendEntriesPipeline.
func (s *Server) HandleRaftCommand(command interface{}, data io.Reader, flow flow, msgType uint8, rpcId uint64, waitForResponse bool, start time.Time) error {
	ch := make(chan raft.RPCResponse, 1)
	rpc := raft.RPC{
		Command:  command,
		RespChan: ch,
		Reader:   data,
	}
	s.transport.rpcChan <- rpc

	if waitForResponse {
		return s.GetResponseFromChannel(ch, flow, msgType, rpcId, start)
	}

	// Add the response channel to the list of pending responses
	s.pendingSnapshotResponses[flow] = ch

	// Send a dummy response back to the Machnet Channel
	response := RpcMessage{
		MsgType: DummyResponse,
		RpcId:   rpcId,
		Payload: []byte{},
	}

	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleAppendEntriesRequest(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	var appendEntriesRequest raft.AppendEntriesRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&appendEntriesRequest); err != nil {
		glog.Errorf("HandleAppendEntriesRequest: failed to decode payload: %v", err)
		return err
	}

	return s.HandleRaftCommand(&appendEntriesRequest, nil, flow, AppendEntriesRequest, rpcId, true, start)
}

func (s *Server) HandleRequestVoteRequest(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	var requestVoteRequest raft.RequestVoteRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&requestVoteRequest); err != nil {
		return err
	}

	return s.HandleRaftCommand(&requestVoteRequest, nil, flow, RequestVoteRequest, rpcId, true, start)
}

func (s *Server) HandleTimeoutNowRequest(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	var timeoutNowRequest raft.TimeoutNowRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&timeoutNowRequest); err != nil {
		return err
	}

	return s.HandleRaftCommand(&timeoutNowRequest, nil, flow, TimeoutNowRequest, rpcId, true, start)
}

func (s *Server) HandleInstallSnapshotRequestStart(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	var installSnapshotRequest raft.InstallSnapshotRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&installSnapshotRequest); err != nil {
		return err
	}

	snapshotReaderObj := snapshotReader{
		buf: make([]byte, installSnapshotRequest.Size),
	}

	s.snapshotMutex.Lock()
	s.snapshotReaders[flow] = &snapshotReaderObj
	s.snapshotMutex.Unlock()

	return s.HandleRaftCommand(&installSnapshotRequest, &snapshotReaderObj, flow, InstallSnapshotRequestStart, rpcId, false, start)
}

func (s *Server) HandleInstallSnapshotRequestBuffer(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	s.snapshotMutex.Lock()
	snapshotReaderObj := s.snapshotReaders[flow]
	_, err := snapshotReaderObj.Write(payload)
	if err != nil {
		return err
	}
	s.snapshotMutex.Unlock()

	response := RpcMessage{
		MsgType: InstallSnapshotRequestBufferResponse,
		RpcId:   rpcId,
		Payload: []byte{},
	}

	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleInstallSnapshotRequestClose(rpcId uint64, flow flow, start time.Time) error {

	s.snapshotMutex.Lock()
	snapshotReaderObj := s.snapshotReaders[flow]
	snapshotReaderObj.numReady = 0
	s.snapshotMutex.Unlock()

	ch := s.pendingSnapshotResponses[flow]
	delete(s.pendingSnapshotResponses, flow)

	return s.GetResponseFromChannel(ch, flow, InstallSnapshotRequestClose, rpcId, start)
}

func (s *Server) HandleAppendEntriesPipelineStart(rpcId uint64, flow flow, start time.Time) error {

	ch := make(chan raft.RPCResponse, 1024)

	s.pipelineMutex.Lock()
	s.pendingPipelineResponses[flow] = PendingResponse{
		ch:         ch,
		numPending: 0,
		toClose:    false,
	}
	s.pipelineMutex.Unlock()

	response := RpcMessage{
		MsgType: AppendEntriesPipelineStartResponse,
		RpcId:   rpcId,
		Payload: []byte{},
	}

	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleAppendEntriesPipeline(payload []byte, rpcId uint64, flow flow, start time.Time) error {

	var buff bytes.Buffer
	//dec := gob.NewDecoder(&buff)
	dec := codec.NewDecoder(&buff, &codec.MsgpackHandle{})
	var appendEntriesRequest raft.AppendEntriesRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&appendEntriesRequest); err != nil {
		return err
	}

	//s.pipelineMutex.Lock()
	//defer s.pipelineMutex.Unlock()
	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
		//pendingResponse.numPending += 1
		//s.pendingPipelineResponses[flow] = pendingResponse

		rpc := raft.RPC{
			Command:  &appendEntriesRequest,
			RespChan: pendingResponse.ch,
			Reader:   nil,
		}

		_, ok := rpc.Command.(raft.WithRPCHeader)
		if !ok {
			glog.Errorf("HandleAppendEntriesPipeline: appendEntriesRequest does not have a WithRPCHeader")
		}
		s.transport.rpcChan <- rpc
		// wait for answer
		resp := <-pendingResponse.ch
		if resp.Error != nil {
			glog.Error("GetResponseFromChannel: couldn't handle Rpc; error: ", resp.Error)
			return resp.Error
		}

		var buff bytes.Buffer
		//enc := gob.NewEncoder(&buff)
		enc := codec.NewEncoder(&buff, &codec.MsgpackHandle{})
		payload := resp.Response.(*raft.AppendEntriesResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}
		msgType := AppendEntriesPipelineResponse
		response := RpcMessage{
			MsgType: msgType,
			RpcId:   rpcId,
			Payload: buff.Bytes(),
		}
		if err := s.SendMachnetResponse(response, flow, start); err != nil {
			glog.Errorf("GetResponseFromChannel: failed to SendMachnetResponse: %v", err)
			return err
		}
		//if err := s.GetResponseFromChannel(pendingResponse.ch, flow, AppendEntriesPipeline, rpcId, start); err != nil {
		//	return err
		//}
	}

	//s.pipelineMutex.Unlock()

	// send it back

	//response := RpcMessage{
	//	MsgType: AppendEntriesPipelineSendResponse,
	//	RpcId:   rpcId,
	//	Payload: []byte{},
	//}
	//return s.SendMachnetResponse(response, flow, start)
	return nil
}

//func (s *Server) HandleAppendEntriesPipelineRecv(rpcId uint64, flow flow, start time.Time) error {
//
//	s.pipelineMutex.Lock()
//	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
//		pendingResponse.numPending -= 1
//		s.pendingPipelineResponses[flow] = pendingResponse
//
//		if err := s.GetResponseFromChannel(pendingResponse.ch, flow, AppendEntriesPipelineRecv, rpcId, start); err != nil {
//			return err
//		}
//
//		if pendingResponse.numPending == 0 && pendingResponse.toClose {
//			delete(s.pendingPipelineResponses, flow)
//			close(pendingResponse.ch)
//		}
//	}
//	s.pipelineMutex.Unlock()
//
//	return nil
//}

func (s *Server) HandleAppendEntriesPipelineClose(rpcId uint64, flow flow, start time.Time) error {

	s.pipelineMutex.Lock()
	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
		if pendingResponse.numPending > 0 {
			pendingResponse.toClose = true
		} else {
			delete(s.pendingPipelineResponses, flow)
			close(pendingResponse.ch)
		}
	}
	s.pipelineMutex.Unlock()

	response := RpcMessage{
		MsgType: AppendEntriesPipelineCloseResponse,
		RpcId:   rpcId,
		Payload: []byte{},
	}
	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleRPCs() {
	for {
		requestBuff := make([]byte, maxMessageLength)
		recvBytes, flow := machnet.Recv(s.transport.receiveChannelCtx, &requestBuff[0], maxMessageLength, true)
		if recvBytes < 0 {
			glog.Errorf("Failed to receive message from Machnet Channel.")
			continue
		} else if recvBytes == 0 {
			//runtime.Gosched()
			continue
		}

		start := time.Now()

		request, err := DecodeRPCMessage(requestBuff[:recvBytes])
		if err != nil {
			glog.Errorf("Failed to decode rpcMessage.")
			continue
		}

		if IsHeartbeat(request.Payload) {
			glog.Info("Received Heartbeat")
			response := RpcMessage{MsgType: DummyResponse, Payload: []byte{}}

			if err := s.SendMachnetResponse(response, flow, start); err != nil {
				glog.Errorf("Failed to send heartbeat response back: %+v", err)
			}
		}
		switch request.MsgType {

		case AppendEntriesRequest:
			err := s.HandleAppendEntriesRequest(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesRequest failed: %v", err)
			}

		case RequestVoteRequest:
			err := s.HandleRequestVoteRequest(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Error("HandleRequestVoteRequest failed: %v", err)
			}

		case TimeoutNowRequest:
			err := s.HandleTimeoutNowRequest(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleTimeoutNowRequest failed: %v", err)
			}

		case InstallSnapshotRequestStart:
			err := s.HandleInstallSnapshotRequestStart(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestStart failed: %v", err)
			}

		case InstallSnapshotRequestBuffer:
			err := s.HandleInstallSnapshotRequestBuffer(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestBuffer failed: %v", err)
			}

		case InstallSnapshotRequestClose:
			err := s.HandleInstallSnapshotRequestClose(request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestClose failed: %v", err)
			}

		case AppendEntriesPipelineStart:
			err := s.HandleAppendEntriesPipelineStart(request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineStart failed: %v", err)
			}

		case AppendEntriesPipeline:
			err := s.HandleAppendEntriesPipeline(request.Payload, request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipeline failed: %v", err)
			}

		case AppendEntriesPipelineClose:
			err := s.HandleAppendEntriesPipelineClose(request.RpcId, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineClose failed: %v", err)
			}

		default:
			glog.Errorf("Unknown message type.")
		}
	}
}
