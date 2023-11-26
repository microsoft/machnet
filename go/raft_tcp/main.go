package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"io"
	"log"
	"net"
	"net/http"
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

func main() {
	flag.Parse()

	kvStore := &sync.Map{}
	fsm := &kvFsm{kvStore}

	node, _, err := NewRaft(*localHostname, fsm)
	if err != nil {
		fmt.Printf("Failed to create Raft")
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
		glog.Info("Current node is a follower")
	}
	//
	hs := httpServer{node, kvStore}

	http.HandleFunc("/set", hs.setHandler)
	http.HandleFunc("/get", hs.getHandler)
	if err := http.ListenAndServe("localhost:"+*appPort, nil); err != nil {
		glog.Fatalf("Main: http server couldn't listen & serve: ", err)
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

type httpServer struct {
	r  *raft.Raft
	db *sync.Map
}

func (hs httpServer) setHandler(w http.ResponseWriter, r *http.Request) {
	defer r.Body.Close()
	bs, err := io.ReadAll(r.Body)
	if err != nil {
		log.Printf("Could not read key-value in http request: %s", err)
		http.Error(w, http.StatusText(http.StatusBadRequest), http.StatusBadRequest)
		return
	}

	start := time.Now()
	future := hs.r.Apply(bs, 500*time.Millisecond)
	if err := future.Error(); err != nil {
		log.Printf("Could not write key-value: %s", err)
		http.Error(w, http.StatusText(http.StatusBadRequest), http.StatusBadRequest)
		return
	}

	e := future.Response()
	if e != nil {
		log.Printf("Could not write key-value, application: %s", e)
		http.Error(w, http.StatusText(http.StatusBadRequest), http.StatusBadRequest)
		return
	}
	glog.Infof("Replication took: %+v\n", time.Since(start))

	w.WriteHeader(http.StatusOK)
}

func (hs httpServer) getHandler(w http.ResponseWriter, r *http.Request) {
	key := r.URL.Query().Get("key")
	value, _ := hs.db.Load(key)
	if value == nil {
		value = ""
	}

	rsp := struct {
		Data string `json:"data"`
	}{value.(string)}
	err := json.NewEncoder(w).Encode(rsp)
	if err != nil {
		log.Printf("Could not encode key-value in http response: %s", err)
		http.Error(w, http.StatusText(http.StatusInternalServerError), http.StatusInternalServerError)
	}
}
