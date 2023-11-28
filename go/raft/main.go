package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"github.com/microsoft/machnet"
)

var (
	localHostname = flag.String("local_hostname", "", "Local hostname in the hosts JSON file.")
	appPort       = flag.Int("app_port", 888, "Port to listen on for application traffic.")
	serverPort    = flag.Int("server_port", 777, "Port to listen on for Raft traffic.")
	numPeers      = flag.Int("num_peers", 2, "Number of peers in the cluster.")
	configJson    = flag.String("config_json", "../servers.json", "Path to the JSON file containing the hosts config.")
	raftDir       = flag.String("raft_data_dir", "data/", "Raft data dir")
	leader        = flag.Bool("leader", false, "Whether to start the node as a leader")
)

const maxRequestSize = 2048

func main() {
	flag.Parse()

	wt := &WordTracker{}

	if ret := machnet.Init(); ret != 0 {
		glog.Fatal("Failed to initialize the Machnet library.")
	}

	raftNode, transport, err := NewRaft(*localHostname, wt)
	if err != nil {
		log.Fatalf("Failed to start raft: %v", err)
	}

	if *leader {
		leaderCh := raftNode.LeaderCh()
		<-leaderCh

		if raftNode.State() == raft.Leader {
			glog.Info("Current node is the leader.")
		} else {
			glog.Info("Current node is a follower.")
		}

		// Connect to all the peers.
		transport.BootstrapPeers(*numPeers)

		// Add the peers to the cluster.
		jsonBytes, err := os.ReadFile(*configJson)
		if err != nil {
			glog.Fatal("Failed to read config file.")
		}

		for i := 1; i < *numPeers; i++ {
			peerId := fmt.Sprintf("node%d", i)
			peerIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", peerId, "ipv4_addr")
			glog.Info("[RAFT Peer] [", peerIp, ":", *serverPort, "]")

			// Send a message to the peer.
			future := raftNode.AddVoter(raft.ServerID(peerId), raft.ServerAddress(peerIp), 0, 0)
			if err := future.Error(); err != nil {
				glog.Error("Failed to add peer to cluster: ", err)
			}

			glog.Info("Successfully added peer to cluster.")
		}

		// Start the application channel and server.
		go StartApplicationServer(wt, raftNode)
	} else {
		glog.Info("Current node is a follower.")
	}

	server := NewServer(transport)
	server.StartServer()
}

func NewRaft(id string, fsm raft.FSM) (*raft.Raft, *TransportApi, error) {
	c := raft.DefaultConfig()
	c.LocalID = raft.ServerID(id)
	// c.LogLevel = "WARN"

	// Increase the timeouts.
	//c.CommitTimeout = 1 * time.Millisecond
	//c.LeaderLeaseTimeout = 1 * time.Minute
	//c.HeartbeatTimeout = 1 * time.Minute
	//c.ElectionTimeout = 1 * time.Minute

	baseDir := filepath.Join(*raftDir, id)
	err := os.MkdirAll(baseDir, os.ModePerm)
	if err != nil {
		glog.Errorf("Couldn't create directories")
		os.Exit(-1)
	}

	store := NewBuffStore(10240)

	logStore := NewBuffStore(10240)

	snapshotStore, err := raft.NewFileSnapshotStore(baseDir, 3, os.Stderr)
	if err != nil {
		return nil, nil, fmt.Errorf(`raft.NewFileSnapshotStore(%q, ...): %v`, baseDir, err)
	}

	var sendChannelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if sendChannelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	var recvChannelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if recvChannelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")

	transport := NewTransport(raft.ServerAddress(localIp), sendChannelCtx, recvChannelCtx, *serverPort, id)

	r, err := raft.NewRaft(c, fsm, logStore, store, snapshotStore, raft.Transport(transport))
	if err != nil {
		return nil, nil, fmt.Errorf("raft.NewRaft: %v", err)
	}

	if *leader {

		raftServers := []raft.Server{
			{
				Suffrage: raft.Voter,
				ID:       raft.ServerID(id),
				Address:  raft.ServerAddress(localIp),
			},
		}

		cfg := raft.Configuration{
			Servers: raftServers,
		}
		f := r.BootstrapCluster(cfg)
		if err := f.Error(); err != nil {
			return nil, nil, fmt.Errorf("raft.Raft.BootstrapCluster: %v", err)
		}
	}

	return r, transport, nil
}

func StartApplicationServer(wt *WordTracker, raftNode *raft.Raft) {

	var channelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if channelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")

	ret := machnet.Listen(channelCtx, localIp, uint(*appPort))
	if ret != 0 {
		glog.Fatal("Failed to listen for incoming connections.")
	}
	glog.Warningf("[APP SERVER LISTENING] [", localIp, ":", *appPort, "]")

	rpcInterface := rpcInterface{wt, raftNode}

	request := make([]byte, maxRequestSize)
	response := new(bytes.Buffer)

	histogram := hdrhistogram.New(1, 1000000, 3)
	lastRecordedTime := time.Now()
	for {
		recvBytes, flow := machnet.Recv(channelCtx, &request[0], maxRequestSize)
		if recvBytes < 0 {
			glog.Fatal("Failed to receive data from client.")
		}

		// Handle the request.
		if recvBytes > 0 {
			start := time.Now()
			index, _ := rpcInterface.AddWord(request[:recvBytes])
			//glog.Warningf("Replicated %s in %d us", string(request[:recvBytes]), time.Since(start).Microseconds())

			tmpFlow := flow
			flow.SrcIp = tmpFlow.DstIp
			flow.DstIp = tmpFlow.SrcIp
			flow.SrcPort = tmpFlow.DstPort
			flow.DstPort = tmpFlow.SrcPort

			response.Reset()
			response.Grow(recvBytes)
			err := binary.Write(response, binary.LittleEndian, index)
			if err != nil {
				glog.Errorf("Failed to create response:", err)
				continue
			}

			payload := response.Bytes()

			ret := machnet.SendMsg(channelCtx, flow, &payload[0], 64)
			if ret != 0 {
				glog.Error("Failed to send data to client.")
			}
			//glog.Warningf("Sent %s 's index [%d] at %+v", string(request[:recvBytes]), index, time.Now())
			elapsed := time.Since(start)
			_ = histogram.RecordValue(elapsed.Nanoseconds())

			if time.Since(lastRecordedTime) > 1*time.Second {
				percentileValues := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
				glog.Warningf("[Processing Time: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us]",
					float64(percentileValues[50.0])/1000, float64(percentileValues[95.0])/1000,
					float64(percentileValues[99.0])/1000, float64(percentileValues[99.9])/1000)
				histogram.Reset()
				lastRecordedTime = time.Now()
			}
		}
	}
}
