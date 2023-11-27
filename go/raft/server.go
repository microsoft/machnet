package main

import (
	"bytes"
	"encoding/gob"
	"errors"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"github.com/microsoft/machnet"
	"io"
	"sync"
)

type Server struct {
	transport *MachnetTransport

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

func NewServer(transport *MachnetTransport) *Server {
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
	ret := machnet.Listen(s.transport.channelCtx, string(s.transport.localIp), uint(s.transport.raftPort))
	if ret != 0 {
		glog.Fatal("Failed to listen for incoming connections")
	}
	glog.Info("[LISTENING] [", s.transport.localIp, ":", s.transport.raftPort, "]")

	// Start listening for RPCs.
	s.HandleRPCs()
}

func (s *Server) HandleRPCs() {

	for {
		// Receive Message from the Machnet Channel
		var rpcType byte
		recvBytes, f := machnet.Recv(s.transport.channelCtx, &rpcType, 1)

		if recvBytes < 0 {
			glog.Errorf("Failed to receive message from Machnet Channel.")
			continue
		} else if recvBytes == 0 {
			continue
		}

		respBuf := make([]byte, maxMessageLength)
		recvBytes, f = machnet.Recv(s.transport.channelCtx, &respBuf[0], maxMessageLength)

		if recvBytes < 0 {
			glog.Errorf("Failed to receive message from Machnet Channel.")
			continue
		} else if recvBytes == 0 {
			continue
		}

		var buf bytes.Buffer
		dec := gob.NewDecoder(&buf)
		if n, _ := buf.Write(respBuf[:recvBytes]); n != recvBytes {
			glog.Errorf("Failed to write to buffer completely")
			continue
		}
		// Create the RPC object
		respCh := make(chan raft.RPCResponse, 1)
		rpc := raft.RPC{
			RespChan: respCh,
		}

		isHeartbeat := false

		switch rpcType {
		case rpcAppendEntries:
			var req raft.AppendEntriesRequest
			if err := dec.Decode(&req); err != nil {
				glog.Errorf("Failed to decode: %v", err)
				continue
			}

			rpc.Command = &req
			leaderAddr := req.Leader
			if len(leaderAddr) == 0 {
				leaderAddr = req.Leader
			}

			// Check if this is a heartbeat
			if req.Term != 0 && leaderAddr != nil &&
				req.PrevLogEntry == 0 && req.PrevLogTerm == 0 &&
				len(req.Entries) == 0 && req.LeaderCommitIndex == 0 {
				isHeartbeat = true
			}

		case rpcRequestVote:
			var req raft.RequestVoteRequest
			if err := dec.Decode(&req); err != nil {
				glog.Error("Failed to handle RequestVote request: %v", err)
				continue
			}
			rpc.Command = &req

		case rpcInstallSnapshot:
			var req raft.InstallSnapshotRequest
			if err := dec.Decode(&req); err != nil {
				glog.Errorf("Failed to handle InstallSnapshot request: %v", err)
				continue
			}
			rpc.Command = &req
		case rpcTimeoutNow:
			var req raft.TimeoutNowRequest
			if err := dec.Decode(&req); err != nil {
				glog.Errorf("Failed to handle TimeoutNow request: %v", err)
				continue
			}
			rpc.Command = &req

		default:
			glog.Errorf("Unknown message type.")
		}

		// Check for heartbeat fast-path
		if isHeartbeat {
			s.transport.heartbeatFnLock.Lock()
			fn := s.transport.heartbeatFn
			s.transport.heartbeatFnLock.Unlock()
			if fn != nil {
				fn(rpc)
				goto RESP
			}
		}
		// Dispatch the RPC
		select {
		case s.transport.consumeCh <- rpc:
		case <-s.transport.shutdownCh:
			glog.Error("ErrTransportShutdown")
		}
	RESP:
		select {
		case resp := <-respCh:
			respErr := ""
			if resp.Error != nil {
				respErr = resp.Error.Error()
			}
			var buf bytes.Buffer
			enc := gob.NewEncoder(&buf)
			if err := enc.Encode(respErr); err != nil {
				glog.Errorf("Failed to encode respErr")
				continue
			}
			msgBytes := buf.Bytes()
			if ret := machnet.SendMsg(s.transport.channelCtx, f, &msgBytes[0], uint(len(msgBytes))); ret != 0 {
				glog.Error("Failed to send machnet rpc response (respErr)")
			}
			buf.Reset()
			if err := enc.Encode(resp.Response); err != nil {
				glog.Errorf("Failed to encode respErr")
				continue
			}
			msgBytes = buf.Bytes()
			// Send the response
			if ret := machnet.SendMsg(s.transport.channelCtx, f, &msgBytes[0], uint(len(msgBytes))); ret != 0 {
				glog.Error("Failed to send machnet rpc response (response)")
			}
		case <-s.transport.shutdownCh:
			glog.Error("ErrorTransportShutdown")
			// Wait for response

		}
	}
}
