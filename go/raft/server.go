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

type Server struct {
	transport *TransportApi

	// Assumption: There is only one snapshot per flow.
	snapshotReaders map[flow]*snapshotReader
	snapshotMutex   sync.Mutex

	// Assumption: There is only one pending snapshot response per flow.
	pendingSnapshotResponses map[flow]chan raft.RPCResponse

	// Assumption: There is only one pending pipeline channel per flow.
	pendingPipelineResponses map[flow]chan raft.RPCResponse
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

func NewServer(transport *TransportApi) *Server {
	return &Server{
		transport:       transport,
		snapshotReaders: make(map[flow]*snapshotReader),

		pendingSnapshotResponses: make(map[flow]chan raft.RPCResponse),
		pendingPipelineResponses: make(map[flow]chan raft.RPCResponse),
	}
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
	dec := gob.NewDecoder(&buff)

	// Decode the message
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
	// Encode the response
	var buff bytes.Buffer
	enc := gob.NewEncoder(&buff)

	if err := enc.Encode(response); err != nil {
		glog.Errorf("SendMachnetResponse: failed to encode response: %v", err)
		return err
	}

	// Send the response back to the Machnet Channel
	responseBytes := buff.Bytes()
	responseLen := len(responseBytes)

	// Swap the flow's source and destination addresses
	tmpFlow := flow
	flow.SrcIp = tmpFlow.DstIp
	flow.DstIp = tmpFlow.SrcIp
	flow.SrcPort = tmpFlow.DstPort
	flow.DstPort = tmpFlow.SrcPort

	// glog.Info("Sending response to Machnet Channel: ", response.MsgType, " with length: ", responseLen)

	elapsed := time.Since(start)
	glog.Info("SendMachnetResponse: Total Rpc took: ", elapsed.Microseconds(), " us")
	//glog.Infof("SendMachnetResponse: putting response on wire: %+v", responseBytes)
	// Send to the remote host on the flow.
	ret := machnet.SendMsg(s.transport.receiveChannelCtx, flow, &responseBytes[0], uint(responseLen))
	if ret != 0 {
		return errors.New("SendMachnetResponse: failed to send message to remote host")
	}

	return nil
}

func (s *Server) GetResponseFromChannel(ch <-chan raft.RPCResponse, flow flow, msgType uint8, start time.Time) error {
	raftStart := time.Now()
	glog.Info("GetResponseFromChannel: waiting for response from Raft channel...")
	resp := <-ch
	if resp.Error != nil {
		glog.Error("GetResponseFromChannel: couldn't handle Rpc; error: ", resp.Error)
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
		return errors.New("GetResponseFromChannel: Unknown message type")
	}

	// Construct the rpcMessage response.
	response := RpcMessage{
		MsgType: Response,
		Payload: buff.Bytes(),
	}
	//glog.Infof("GetResponseFromChannel: MsgType: %v Payload: %+v", response.MsgType, response.Payload)
	if err := s.SendMachnetResponse(response, flow, start); err != nil {
		glog.Errorf("GetResponseFromChannel: failed to SendMachnetResponse: %v", err)
		return err
	}

	raftElapsed := time.Since(raftStart)
	glog.Info("GetResponseFromChannel: Raft Rpc took: ", raftElapsed.Microseconds(), " us")

	return nil
}

// HandleRaftCommand Send a Raft command to the Raft channel.
// To be used for all operations except AppendEntriesPipeline.
func (s *Server) HandleRaftCommand(command interface{}, data io.Reader, flow flow, msgType uint8, waitForResponse bool, start time.Time) error {
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

	// Send a dummy response back to the Machnet Channel
	response := RpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	//glog.Infof("HandleRaftCommand: sent dummy response: %v msgType was: %v", response, msgType)
	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleAppendEntriesRequest(payload []byte, flow flow, start time.Time) error {
	// Decode the payload to get the AppendEntriesRequest struct
	var buff bytes.Buffer
	dec := gob.NewDecoder(&buff)

	var appendEntriesRequest raft.AppendEntriesRequest
	if n, _ := buff.Write(payload); n != len(payload) {
		return errors.New("failed to write payload into buffer")
	}

	if err := dec.Decode(&appendEntriesRequest); err != nil {
		glog.Errorf("HandleAppendEntriesRequest: failed to decode payload: %v", err)
		return err
	}

	// Get the previous index from the AppendEntriesRequest
	// prevIndex := appendEntriesRequest.PrevLogEntry
	// glog.Info("Received AppendEntriesRequest with PrevLogEntry: ", prevIndex)

	return s.HandleRaftCommand(&appendEntriesRequest, nil, flow, AppendEntriesRequest, true, start)
}

func (s *Server) HandleRequestVoteRequest(payload []byte, flow flow, start time.Time) error {
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

func (s *Server) HandleTimeoutNowRequest(payload []byte, flow flow, start time.Time) error {
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

func (s *Server) HandleInstallSnapshotRequestStart(payload []byte, flow flow, start time.Time) error {
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

func (s *Server) HandleInstallSnapshotRequestBuffer(payload []byte, flow flow, start time.Time) error {
	// Find the snapshotReader corresponding to the flow and write the payload to it
	s.snapshotMutex.Lock()
	snapshotReaderObj := s.snapshotReaders[flow]
	_, err := snapshotReaderObj.Write(payload)
	if err != nil {
		return err
	}
	s.snapshotMutex.Unlock()

	// Send a dummy response back to the Machnet Channel
	response := RpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	return s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleInstallSnapshotRequestClose(flow flow, start time.Time) error {
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

func (s *Server) HandleAppendEntriesPipelineStart(flow flow, start time.Time) error {
	// Construct a channel to send RPCs and add it to the list of pendingPipelineResponses
	ch := make(chan raft.RPCResponse, 1024)

	s.pipelineMutex.Lock()
	s.pendingPipelineResponses[flow] = ch
	s.pipelineMutex.Unlock()

	// Send a dummy response back to the Machnet Channel
	_ = RpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}

	//glog.Infof("HandleAppendEntriesPipelineStart: send dummy response: %+v", response)
	return nil //s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleAppendEntriesPipelineSend(payload []byte, flow flow, start time.Time) error {
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
	ch := s.pendingPipelineResponses[flow]
	rpc := raft.RPC{
		Command:  &appendEntriesRequest,
		RespChan: ch,
		Reader:   nil,
	}

	_, ok := rpc.Command.(raft.WithRPCHeader)
	if !ok {
		glog.Error("AppendEntriesRequest does not have a WithRPCHeader")
	}

	s.transport.rpcChan <- rpc

	// Send a dummy response back to the Machnet Channel
	_ = RpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}
	//glog.Infof("HandleAppendEntriesPipelineSend: send dummy response: %+v", response)
	return nil //s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleAppendEntriesPipelineRecv(flow flow, start time.Time) error {
	// Get the channel corresponding to the flow
	ch := s.pendingPipelineResponses[flow]

	// Send response back to the Machnet Channel
	return s.GetResponseFromChannel(ch, flow, AppendEntriesPipelineRecv, start)
}

func (s *Server) HandleAppendEntriesPipelineClose(flow flow, start time.Time) error {
	// Get the channel corresponding to the flow and remove it from the list of pendingPipelineResponses
	ch := s.pendingPipelineResponses[flow]
	delete(s.pendingPipelineResponses, flow)
	close(ch)

	// Send a dummy response back to the Machnet Channel
	_ = RpcMessage{
		MsgType: Response,
		Payload: []byte{},
	}
	//glog.Infof("HandleAppendEntriesPipelineClose: sent dummy response: %+v", response)
	return nil //s.SendMachnetResponse(response, flow, start)
}

func (s *Server) HandleRPCs() {

	for {
		// Receive Message from the Machnet Channel
		requestBuff := make([]byte, maxMessageLength)
		recvBytes, flow := machnet.Recv(s.transport.receiveChannelCtx, &requestBuff[0], maxMessageLength)
		if recvBytes < 0 {
			glog.Errorf("Failed to receive message from Machnet Channel.")
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

		// Depending on the type of message, handle it accordingly
		switch request.MsgType {
		case AppendEntriesRequest:
			err := s.HandleAppendEntriesRequest(request.Payload, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesRequest failed: %v", err)
			}

		case RequestVoteRequest:
			err := s.HandleRequestVoteRequest(request.Payload, flow, start)
			if err != nil {
				glog.Error("HandleRequestVoteRequest failed: %v", err)
			}

		case TimeoutNowRequest:
			err := s.HandleTimeoutNowRequest(request.Payload, flow, start)
			if err != nil {
				glog.Errorf("HandleTimeoutNowRequest failed: %v", err)
			}

		case InstallSnapshotRequestStart:
			err := s.HandleInstallSnapshotRequestStart(request.Payload, flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestStart failed: %v", err)
			}

		case InstallSnapshotRequestBuffer:
			err := s.HandleInstallSnapshotRequestBuffer(request.Payload, flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestBuffer failed: %v", err)
			}

		case InstallSnapshotRequestClose:
			err := s.HandleInstallSnapshotRequestClose(flow, start)
			if err != nil {
				glog.Errorf("HandleInstallSnapshotRequestClose failed: %v", err)
			}

		case AppendEntriesPipelineStart:
			err := s.HandleAppendEntriesPipelineStart(flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineStart failed: %v", err)
			}

		case AppendEntriesPipelineSend:
			err := s.HandleAppendEntriesPipelineSend(request.Payload, flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineSend failed: %v", err)
			}

		case AppendEntriesPipelineRecv:
			err := s.HandleAppendEntriesPipelineRecv(flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineRecv failed: %v", err)
			}

		case AppendEntriesPipelineClose:
			err := s.HandleAppendEntriesPipelineClose(flow, start)
			if err != nil {
				glog.Errorf("HandleAppendEntriesPipelineClose failed: %v", err)
			}

		default:
			glog.Errorf("Unknown message type.")
		}
	}
}
