package main

import (
	"bytes"
	"encoding/gob"
	"errors"
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

type server struct {
	transport *TransportApi

	// Assumption: There is only one snapshot per flow.
	snapshotReaders map[flow]*snapshotReader
	snapshotMutex   sync.Mutex

	// Assumption: There is only one pending snapshot response per flow.
	pendingSnapshotResponses map[flow]chan raft.RPCResponse

	// Assumption: There is only one pending pipeline channel per flow.
	pendingPipelineResponses map[flow]PendingResponse
	pipelineMutex            sync.Mutex
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

func NewServer(transport *TransportApi) *server {
	return &server{
		transport:       transport,
		snapshotReaders: make(map[flow]*snapshotReader),

		pendingSnapshotResponses: make(map[flow]chan raft.RPCResponse),
		pendingPipelineResponses: make(map[flow]PendingResponse),
	}
}

// Listens for incoming connection from leader, and start the Recv thread.
func (s *server) StartServer() {
	// Listen for incoming connections.
	ret := machnet.Listen(s.transport.receiveChannelCtx, string(s.transport.localIp), uint(s.transport.raftPort))
	if ret != 0 {
		glog.Fatal("Failed to listen for incoming connections")
	}
	glog.Info("[LISTENING] [", s.transport.localIp, ":", s.transport.raftPort, "]")

	// Start listening for RPCs.
	s.HandleRPCs()
}

func DecodeRPCMessage(data []byte) (rpcMessage, error) {
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	// Decode the message
	if n, _ := buff.Write(data); n != len(data) {
		return rpcMessage{}, errors.New("failed to write response into buffer")
	}

	var request rpcMessage
	if err := dec.Decode(&request); err != nil {
		return rpcMessage{}, err
	}

	return request, nil
}

func IsHeartbeat(command interface{}) bool {
	req, ok := command.(*raft.AppendEntriesRequest)
	if !ok {
		return false
	}
	return req.Term != 0 && req.PrevLogEntry == 0 && req.PrevLogTerm == 0 && len(req.Entries) == 0 && req.LeaderCommitIndex == 0
}

// SendNSaaSResponse sends a response to the NSaaS Channel.
// The response is sent on the reverse flow.
//
// Parameters:
// response (rpcMessage): The response to be sent.
// flow (flow): The flow that received the original RPC. SendNSaaSResponse will send the response on the reverse flow.
func (s *server) SendNSaaSResponse(response rpcMessage, flow flow, start time.Time) error {
	// Encode the response
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)

	if err := enc.Encode(response); err != nil {
		return err
	}

	// Send the response back to the NSaaS Channel
	responseBytes := buff.Bytes()
	responseLen := len(responseBytes)

	if response.MsgType == AppendEntriesPipelineRecv {
		glog.V(3).Info("Sending AEPRecv response: ", responseBytes)
	}

	// Swap the flow's source and destination addresses
	tmp_flow := flow
	flow.SrcIp = tmp_flow.DstIp
	flow.DstIp = tmp_flow.SrcIp
	flow.SrcPort = tmp_flow.DstPort
	flow.DstPort = tmp_flow.SrcPort

	elapsed := time.Since(start)
	glog.V(4).Info("Total RPC took: ", elapsed.Microseconds(), " us")

	// Send to the remote host on the flow.
	ret := machnet.SendMsg(s.transport.receiveChannelCtx, flow, &responseBytes[0], uint(responseLen))
	if ret != 0 {
		return errors.New("failed to send message to remote host")
	}

	return nil
}

func (s *server) GetResponseFromChannel(ch <-chan raft.RPCResponse, flow flow, msgType uint8, start time.Time) error {
	raftStart := time.Now()
	resp := <-ch
	if resp.Error != nil {
		glog.Error("Error in RPC: ", resp.Error)
		return resp.Error
	}

	// Encode the response
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)

	// Based on MsgType, construct the rpcMessage payload
	switch msgType {
	case AppendEntriesRequest:
		payload := resp.Response.(*raft.AppendEntriesResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	case RequestVoteRequest:
		payload := resp.Response.(*raft.RequestVoteResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	case TimeoutNowRequest:
		payload := resp.Response.(*raft.TimeoutNowResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	case InstallSnapshotRequestStart:
		payload := resp.Response.(*raft.InstallSnapshotResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	case InstallSnapshotRequestClose:
		payload := resp.Response.(*raft.InstallSnapshotResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	case AppendEntriesPipelineRecv:
		payload := resp.Response.(*raft.AppendEntriesResponse)
		if err := enc.Encode(*payload); err != nil {
			return err
		}

	default:
		return errors.New("unknown message type")
	}

	// Construct the rpcMessage response.
	response := rpcMessage{
		MsgType: Response,
		Payload: buff.Bytes(),
	}
	if msgType == AppendEntriesRequest {
		glog.V(3).Info("Length of response payload: ", len(response.Payload))
	}

	if err := s.SendNSaaSResponse(response, flow, start); err != nil {
		return err
	}

	raftElapsed := time.Since(raftStart)
	glog.V(4).Info("Raft RPC took: ", raftElapsed.Microseconds(), " us")

	return nil
}

// Send a Raft command to the Raft channel.
// To be used for all operations except AppendEntriesPipeline.
func (s *server) HandleRaftCommand(command interface{}, data io.Reader, flow flow, msgType uint8, waitForResponse bool, start time.Time) error {
	ch := make(chan raft.RPCResponse, 1)
	rpc := raft.RPC{
		Command:  command,
		RespChan: ch,
		Reader:   data,
	}
	s.transport.rpcChan <- rpc

	if waitForResponse {
		return s.GetResponseFromChannel(ch, flow, msgType, start)
	}

	// Add the response channel to the list of pending responses
	s.pendingSnapshotResponses[flow] = ch

	// Send a dummy response back to the NSaaS Channel
	response := rpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	return s.SendNSaaSResponse(response, flow, start)
}

func (s *server) HandleAppendEntriesRequest(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the AppendEntriesRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var appendEntriesRequest raft.AppendEntriesRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&appendEntriesRequest); err != nil {
		return err
	}

	// Get the previous index from the AppendEntriesRequest
	// prevIndex := appendEntriesRequest.PrevLogEntry
	// glog.Info("Received AppendEntriesRequest with PrevLogEntry: ", prevIndex)

	return s.HandleRaftCommand(&appendEntriesRequest, nil, flow, AppendEntriesRequest, true, start)
}

func (s *server) HandleRequestVoteRequest(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the RequestVoteRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var requestVoteRequest raft.RequestVoteRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&requestVoteRequest); err != nil {
		return err
	}

	return s.HandleRaftCommand(&requestVoteRequest, nil, flow, RequestVoteRequest, true, start)
}

func (s *server) HandleTimeoutNowRequest(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the TimeoutNowRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var timeoutNowRequest raft.TimeoutNowRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&timeoutNowRequest); err != nil {
		return err
	}

	return s.HandleRaftCommand(&timeoutNowRequest, nil, flow, TimeoutNowRequest, true, start)
}

func (s *server) HandleInstallSnapshotRequestStart(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the InstallSnapshotRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var installSnapshotRequest raft.InstallSnapshotRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&installSnapshotRequest); err != nil {
		return err
	}

	// Create a snapshotReader and add it to the list of snapshotReaders
	snapshotReaderObj := snapshotReader{
		buf: make([]byte, installSnapshotRequest.Size),
	}

	// Add the snapshotReader to the list of snapshotReaders
	s.snapshotMutex.Lock()
	s.snapshotReaders[flow] = &snapshotReaderObj
	s.snapshotMutex.Unlock()

	return s.HandleRaftCommand(&installSnapshotRequest, &snapshotReaderObj, flow, InstallSnapshotRequestStart, false, start)
}

func (s *server) HandleInstallSnapshotRequestBuffer(payload []byte, flow flow, start time.Time) error {
	// Find the snapshotReader corresponding to the flow and write the payload to it
	s.snapshotMutex.Lock()
	snapshotReaderObj := s.snapshotReaders[flow]
	snapshotReaderObj.Write(payload)
	s.snapshotMutex.Unlock()

	// Send a dummy response back to the NSaaS Channel
	response := rpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	return s.SendNSaaSResponse(response, flow, start)
}

func (s *server) HandleInstallSnapshotRequestClose(flow flow, start time.Time) error {
	// Close the snapshotReader and remove it from the list of snapshotReaders
	s.snapshotMutex.Lock()
	snapshotReaderObj := s.snapshotReaders[flow]
	snapshotReaderObj.numReady = 0
	s.snapshotMutex.Unlock()

	// Get the channel corresponding to the flow and remove it from the list of pendingSnapshotResponses
	ch := s.pendingSnapshotResponses[flow]
	delete(s.pendingSnapshotResponses, flow)

	return s.GetResponseFromChannel(ch, flow, InstallSnapshotRequestClose, start)
}

func (s *server) HandleAppendEntriesPipelineStart(flow flow, start time.Time) error {
	// Construct a channel to send RPCs and add it to the list of pendingPipelineResponses
	ch := make(chan raft.RPCResponse, 1024)

	s.pipelineMutex.Lock()
	s.pendingPipelineResponses[flow] = PendingResponse{
		ch:         ch,
		numPending: 0,
		toClose:    false,
	}
	s.pipelineMutex.Unlock()

	// Send a dummy response back to the NSaaS Channel
	response := rpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	return s.SendNSaaSResponse(response, flow, start)
}

func (s *server) HandleAppendEntriesPipelineSend(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the AppendEntriesRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var appendEntriesRequest raft.AppendEntriesRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&appendEntriesRequest); err != nil {
		return err
	}

	// Send the RPC to the Raft channel
	s.pipelineMutex.Lock()
	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
		pendingResponse.numPending += 1
		s.pendingPipelineResponses[flow] = pendingResponse

		rpc := raft.RPC{
			Command:  &appendEntriesRequest,
			RespChan: pendingResponse.ch,
			Reader:   nil,
		}

		_, ok := rpc.Command.(raft.WithRPCHeader)
		if !ok {
			glog.Error("AppendEntriesRequest does not have a WithRPCHeader")
		}
		s.transport.rpcChan <- rpc
	}
	s.pipelineMutex.Unlock()

	// Send a dummy response back to the NSaaS Channel
	response := rpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}
	return s.SendNSaaSResponse(response, flow, start)
}

func (s *server) HandleAppendEntriesPipelineRecv(flow flow, start time.Time) error {
	// Get the channel corresponding to the flow
	s.pipelineMutex.Lock()
	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
		pendingResponse.numPending -= 1
		s.pendingPipelineResponses[flow] = pendingResponse

		// Send response back to the NSaaS Channel
		err := s.GetResponseFromChannel(pendingResponse.ch, flow, AppendEntriesPipelineRecv, start)
		if err != nil {
			return err
		}

		// Check if the channel needs to be closed
		if pendingResponse.numPending == 0 && pendingResponse.toClose {
			delete(s.pendingPipelineResponses, flow)
			close(pendingResponse.ch)
		}
	}
	s.pipelineMutex.Unlock()

	return nil
}

func (s *server) HandleAppendEntriesPipelineClose(flow flow, start time.Time) error {
	// Get the channel corresponding to the flow and remove it from the list of pendingPipelineResponses
	s.pipelineMutex.Lock()

	// Check if there is a pending response
	if pendingResponse, ok := s.pendingPipelineResponses[flow]; ok {
		if pendingResponse.numPending > 0 {
			// If there is a pending response, mark the channel for closing but wait for the receive message
			pendingResponse.toClose = true
		} else {
			// Close the channel
			delete(s.pendingPipelineResponses, flow)
			close(pendingResponse.ch)
		}
	}
	s.pipelineMutex.Unlock()

	// Send a dummy response back to the NSaaS Channel
	response := rpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}
	return s.SendNSaaSResponse(response, flow, start)
}

func (s *server) HandleRPCs() {
	numReceived := 0
	lastRecordedTime := time.Now()
	for {
		// Receive Message from the NSaaS Channel
		requestBuff := make([]byte, maxMessageLength)
		recvBytes, flow := machnet.Recv(s.transport.receiveChannelCtx, &requestBuff[0], maxMessageLength)
		if recvBytes < 0 {
			glog.Errorf("Failed to receive message from NSaaS Channel.")
			continue
		} else if recvBytes == 0 {
			continue
		}

		start := time.Now()

		// Decode the rpcMessage
		request, err := DecodeRPCMessage(requestBuff[:recvBytes])
		if err != nil {
			glog.Errorf("Failed to decode rpcMessage.")
			continue
		}

		numReceived += 1
		glog.V(5).Info("Received message from NSaaS: ", request.MsgType, " size: ", recvBytes, " numRecv: ", numReceived)
		now := time.Now()
		if now.Sub(lastRecordedTime) > 1*time.Second {
			glog.V(4).Info("Received ", numReceived, " messages in the last second.")
			numReceived = 0
			lastRecordedTime = time.Now()
		}

		if IsHeartbeat(request.Payload) {
			glog.Info("Received heartbeat from NSaaS Channel.")

			// Send back a dummy response.
			response := rpcMessage{
				MsgType: Response,
				Payload: []byte{},
			}

			if err := s.SendNSaaSResponse(response, flow, start); err != nil {
				glog.Error("Failed to send heartbeat response to NSaaS Channel.")
			}
		}

		// Depending on the type of message, handle it accordingly
		switch request.MsgType {
		case AppendEntriesRequest:
			s.HandleAppendEntriesRequest(request.Payload, flow, start)

		case RequestVoteRequest:
			s.HandleRequestVoteRequest(request.Payload, flow, start)

		case TimeoutNowRequest:
			s.HandleTimeoutNowRequest(request.Payload, flow, start)

		case InstallSnapshotRequestStart:
			s.HandleInstallSnapshotRequestStart(request.Payload, flow, start)

		case InstallSnapshotRequestBuffer:
			s.HandleInstallSnapshotRequestBuffer(request.Payload, flow, start)

		case InstallSnapshotRequestClose:
			s.HandleInstallSnapshotRequestClose(flow, start)

		case AppendEntriesPipelineStart:
			s.HandleAppendEntriesPipelineStart(flow, start)

		case AppendEntriesPipelineSend:
			s.HandleAppendEntriesPipelineSend(request.Payload, flow, start)

		case AppendEntriesPipelineRecv:
			s.HandleAppendEntriesPipelineRecv(flow, start)

		case AppendEntriesPipelineClose:
			s.HandleAppendEntriesPipelineClose(flow, start)

		default:
			glog.Errorf("Unknown message type.")
		}
	}
}
