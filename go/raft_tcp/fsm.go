package main

import (
	"encoding/json"
	"fmt"
	"github.com/hashicorp/raft"
	"io"
	"sync"
)

type kvFsm struct {
	kvStore *sync.Map
}

type Payload struct {
	Key   string
	Value string
}

func (fsm *kvFsm) Apply(log *raft.Log) any {
	switch log.Type {
	case raft.LogCommand:
		var sp Payload
		err := json.Unmarshal(log.Data, &sp)
		if err != nil {
			return fmt.Errorf("apply: failed to parse payload: %v", err)
		}

		fsm.kvStore.Store(sp.Key, sp.Value)
	default:
		return fmt.Errorf("apply: unknown raft log type: %#v", log.Type)
	}

	return nil
}

type snapshotNoop struct{}

func (sn snapshotNoop) Persist(_ raft.SnapshotSink) error { return nil }
func (sn snapshotNoop) Release()                          {}

func (fsm *kvFsm) Snapshot() (raft.FSMSnapshot, error) {
	return snapshotNoop{}, nil
}

func (fsm *kvFsm) Restore(rc io.ReadCloser) error {
	// deleting first isn't really necessary since there's no exposed DELETE operation anyway.
	// so any changes over time will just get naturally overwritten

	decoder := json.NewDecoder(rc)

	for decoder.More() {
		var sp Payload
		err := decoder.Decode(&sp)
		if err != nil {
			return fmt.Errorf("restore: failed to decode payload: %v", err)
		}

		fsm.kvStore.Store(sp.Key, sp.Value)
	}

	return rc.Close()
}
