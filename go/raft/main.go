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

const maxWordLength = 64

func main() {
	flag.Parse()

	wt := &WordTracker{}

	// Initialize the Machnet library.
	if ret := machnet.Init(); ret != 0 {
		glog.Fatal("Failed to initialize the Machnet library.")
	}

	raftNode, transport, err := NewRaft(*localHostname, wt)
	if err != nil {
		log.Fatalf("failed to start raft: %v", err)
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
	//c.LeaderLeaseTimeout = 5 * time.Second
	//c.HeartbeatTimeout = 5 * time.Second
	//c.ElectionTimeout = 60 * time.Second

	baseDir := filepath.Join(*raftDir, id)
	err := os.MkdirAll(baseDir, os.ModePerm)
	if err != nil {
		glog.Errorf("Couldn't create directories")
		os.Exit(-1)
	}

	// store, err := boltdb.NewBoltStore(filepath.Join(baseDir, "stable.dat"))
	// if err != nil {
	// 	return nil, nil, fmt.Errorf(`boltdb.NewBoltStore(%q): %v`, filepath.Join(baseDir, "stable.dat"), err)
	// }
	store := NewBuffStore(10240)

	// logStore, err := boltdb.NewBoltStore(filepath.Join(baseDir, "logs.dat"))
	// if err != nil {
	// 	return nil, nil, fmt.Errorf(`boltdb.NewBoltStore(%q): %v`, filepath.Join(baseDir, "logs.dat"), err)
	// }
	logStore := NewBuffStore(10240)

	snapshotStore, err := raft.NewFileSnapshotStore(baseDir, 3, os.Stderr)
	if err != nil {
		return nil, nil, fmt.Errorf(`raft.NewFileSnapshotStore(%q, ...): %v`, baseDir, err)
	}

	// Define two ChannelCtx, one for sending Raft RPCs and other for receiving.
	var sendChannelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if sendChannelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	var recvChannelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if recvChannelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	// Read the contents of file config_json into a byte array.
	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	// Parse the json file to get the localIp.
	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")

	// Create a new Machnet Transport.
	transport := NewTransport(raft.ServerAddress(localIp), sendChannelCtx, recvChannelCtx, *serverPort, id)

	r, err := raft.NewRaft(c, fsm, logStore, store, snapshotStore, raft.Transport(transport))
	if err != nil {
		return nil, nil, fmt.Errorf("raft.NewRaft: %v", err)
	}

	// Bootstrap the cluster if requested.
	if *leader {
		// Create a list of raft.Server objects.
		raftServers := []raft.Server{
			{
				Suffrage: raft.Voter,
				ID:       raft.ServerID(id),
				Address:  raft.ServerAddress(localIp),
			},
		}

		// Add the peers to the list.
		// for i := 1; i <= *numPeers; i++ {
		// 	peerId := fmt.Sprintf("node%d", i)
		// 	peerIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", peerId, "ipv4_addr")
		// 	raftServers = append(raftServers, raft.Server{
		// 		Suffrage: raft.Voter,
		// 		ID:       raft.ServerID(peerId),
		// 		Address:  raft.ServerAddress(peerIp),
		// 	})
		// }

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
	// Define a pointer variable channel_ctx to store the output of C.machnet_attach()
	var channelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?
	if channelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	// Read the contents of file config_json into a byte array.
	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	// Parse the json file to get the localIp.
	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")

	// Create a new Machnet Transport.
	ret := machnet.Listen(channelCtx, localIp, uint(*appPort))
	if ret != 0 {
		glog.Fatal("Failed to listen for incoming connections.")
	}
	glog.Warningf("[APP SERVER LISTENING] [", localIp, ":", *appPort, "]")

	// Create the rpcInterface object.
	rpcInterface := rpcInterface{wt, raftNode}

	// Buffers to store the request and response.
	request := make([]byte, maxWordLength)
	response := new(bytes.Buffer)
	// Continuously accept incoming requests from client, and handle them.
	histogram := hdrhistogram.New(1, 1000000, 3)
	lastRecordedTime := time.Now()
	for {
		recvBytes, flow := machnet.Recv(channelCtx, &request[0], maxWordLength)
		if recvBytes < 0 {
			glog.Fatal("Failed to receive data from client.")
		}

		// Handle the request.
		if recvBytes > 0 {
			glog.Warningf("Received %s at %+v", string(request[:recvBytes]), time.Now())
			start := time.Now()
			index, _ := rpcInterface.AddWord(string(request[:recvBytes]))
			glog.Warningf("Replicated %s in %d us", string(request[:recvBytes]), time.Since(start).Microseconds())
			//elapsed := time.Since(start)

			// Swap the source and destination IP addresses.
			tmpFlow := flow
			flow.SrcIp = tmpFlow.DstIp
			flow.DstIp = tmpFlow.SrcIp
			flow.SrcPort = tmpFlow.DstPort
			flow.DstPort = tmpFlow.SrcPort

			// Send the index of the word to the client.
			// Make a byte array payload of 64  bytes.
			response.Reset()
			response.Grow(64)
			err := binary.Write(response, binary.LittleEndian, index)
			if err != nil {
				// handle error
				glog.Errorf("Failed to create response:", err)
				continue
			}

			payload := response.Bytes()

			ret := machnet.SendMsg(channelCtx, flow, &payload[0], 64)
			if ret != 0 {
				glog.Error("Failed to send data to client.")
			}
			glog.Warningf("Sent %s 's index [%d] at %+v", string(request[:recvBytes]), index, time.Now())
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
