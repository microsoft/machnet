package main

import (
	"bufio"
	"flag"
	"fmt"
	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
)

var (
	localHostname = flag.String("local_hostname", "", "Local hostname in the hosts JSON file.")
	appPort       = flag.String("app_port", "888", "Port to listen on for application traffic.")
	numPeers      = flag.Int("num_peers", 2, "Number of peers in the cluster.")
	configJson    = flag.String("config_json", "./servers.json", "Path to the JSON file containing the hosts config.")
	raftDir       = flag.String("raft_data_dir", "data/", "Raft data dir")
	leader        = flag.Bool("leader", false, "Whether to start the node as a leader")
)

type appServer struct {
	raft    *raft.Raft
	kvStore *sync.Map
}

func main() {
	flag.Parse()

	kvStore := &sync.Map{}
	fsm := &kvFsm{kvStore}

	node, _, err := NewRaft(*localHostname, fsm)
	if err != nil {
		glog.Fatalf("Main: failed to create new Raft cluster: %v", err)
	}

	if *leader {
		leaderCh := node.LeaderCh()
		<-leaderCh

		if node.State() == raft.Leader {
			glog.Info("Main: current node is the leader")
		} else {
			glog.Info("Main: current node is a follower")
		}

		if err != nil {
			glog.Fatalf("Main: failed to read config file")
		}

		for i := 1; i < *numPeers; i++ {
			peerId := fmt.Sprintf("node%d", i)
			raftAddress, err := getRaftAddress(peerId)
			if err != nil {
				glog.Error("Main: failed to construct Raft peer address")
				continue
			}
			glog.Infof("Main: adding Raft peer %s to cluster ... ", raftAddress)
			f := node.AddVoter(raft.ServerID(peerId), raft.ServerAddress(raftAddress), 0, 0)
			if err := f.Error(); err != nil {
				glog.Errorf("Main: failed to add peer (%s) to cluster: %+v", raftAddress, err)
			}
			glog.Infof("Main: added Raft peer %s to cluster", raftAddress)
		}

	} else {
		glog.Info("Main: current node is a follower")
	}

	app := appServer{node, kvStore}

	histogram := hdrhistogram.New(1, 1000000, 3)
	lastRecordedTime := time.Now()

	ln, err := net.Listen("tcp", ":"+*appPort)
	if err != nil {
		glog.Fatalf("Main: failed to listen : %+v", err)
	}
	defer func(ln net.Listener) {
		err := ln.Close()
		if err != nil {
			glog.Errorf("Main: failed to close listener socket: %+v", err)
		}
	}(ln)

	for {
		conn, err := ln.Accept()
		if err != nil {
			glog.Warningf("Main: failed to accept connection: %+v", err)
			if err := conn.Close(); err != nil {
				glog.Errorf("Main: failed to close connection: %+v", err)
			}
			continue
		}

		reader := bufio.NewReader(conn)

		for {
			rawBytes, err := reader.ReadBytes('\n')
			if err != nil {
				glog.Errorf("Main: couldn't read bytes: %+v", err)
				break
			}

			start := time.Now()

			future := app.raft.Apply(rawBytes, 500*time.Millisecond)
			if err := future.Error(); err != nil {
				glog.Errorf("Main: failed to replicate : %s", err)
			}

			if _, err := conn.Write(rawBytes); err != nil {
				glog.Errorf("Main: failed to write back: %+v", err)
			}

			if err := histogram.RecordValue(time.Since(start).Microseconds()); err != nil {
				glog.Errorf("Main: failed to record to histogram: %v", err)
			}

			if time.Since(lastRecordedTime) > 1*time.Second {
				percentileValues := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
				glog.Warningf("Processing Time: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us",
					float64(percentileValues[50.0]), float64(percentileValues[95.0]),
					float64(percentileValues[99.0]), float64(percentileValues[99.9]))
				histogram.Reset()
				lastRecordedTime = time.Now()
			}
		}

		glog.Infof("Main: closing connection to client: %+v", conn)
		if err := conn.Close(); err != nil {
			glog.Errorf("Main: failed tp close connection: %+v", conn)
		}
	}
}

func getRaftAddress(peerId string) (string, error) {
	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		return "", fmt.Errorf("NewRaft: failed to read config file: %v", err)
	}
	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", peerId, "ipv4_addr")
	raftPort, _ := jsonparser.GetString(jsonBytes, "hosts_config", peerId, "raft_port")
	return localIp + ":" + raftPort, nil
}

func NewRaft(id string, fsm *kvFsm) (*raft.Raft, *raft.NetworkTransport, error) {
	baseDir := filepath.Join(*raftDir, id)
	if err := os.MkdirAll(baseDir, os.ModePerm); err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to create data directory: %s", err)
	}

	store := NewBuffStore(1024)
	logStore := NewBuffStore(1024)
	snapShotStore, err := raft.NewFileSnapshotStore(baseDir, 3, os.Stderr)
	if err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to create snapshot store: %v", err)
	}

	raftCfg := raft.DefaultConfig()
	raftCfg.LocalID = raft.ServerID(id)

	// Read the contents of file config_json into a byte array.
	raftAddress, err := getRaftAddress(*localHostname)
	if err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to construct raft address")
	}
	tcpAddr, err := net.ResolveTCPAddr("tcp", raftAddress)
	if err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to resolve TCP address %s: %v", raftAddress, err)
	}

	transport, err := raft.NewTCPTransport(raftAddress, tcpAddr, 10, time.Second*10, os.Stderr)
	if err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to create new TCP transport: %v", err)
	}

	r, err := raft.NewRaft(raftCfg, fsm, logStore, store, snapShotStore, transport)
	if err != nil {
		return nil, nil, fmt.Errorf("NewRaft: failed to create new Raft: %v", err)
	}
	if *leader {

		raftServers := []raft.Server{
			{
				Suffrage: raft.Voter,
				ID:       raft.ServerID(id),
				Address:  raft.ServerAddress(raftAddress),
			},
		}

		f := r.BootstrapCluster(raft.Configuration{
			Servers: raftServers,
		})
		if err := f.Error(); err != nil {
			return nil, nil, fmt.Errorf("NewRaft: failed to bootstrap cluster: %v", err)
		}
	}
	return r, transport, nil
}
