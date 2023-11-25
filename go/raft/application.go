package main

import (
	"errors"
	"fmt"
	"github.com/golang/glog"
	"io"
	"strings"
	"sync"
	"time"

	"github.com/hashicorp/raft"
)

// WordTracker keeps track of the three longest words it ever saw.
type WordTracker struct {
	mtx   sync.RWMutex
	words [3]string
}

type rpcInterface struct {
	WordTracker *WordTracker
	raft        *raft.Raft
}

type snapshot struct {
	words []string
}

type wordsResponse struct {
	readAtIndex uint64
	words       []string
}

var _ raft.FSM = &WordTracker{}

// compareWords returns true if "a" is longer (lexicography breaking ties).
func compareWords(a, b string) bool {
	if len(a) == len(b) {
		return a < b
	}
	return len(a) > len(b)
}

func cloneWords(words [3]string) []string {
	var ret [3]string
	copy(ret[:], words[:])
	return ret[:]
}

func (f *WordTracker) Apply(l *raft.Log) interface{} {
	f.mtx.Lock()
	defer f.mtx.Unlock()
	w := string(l.Data)
	for i := 0; i < len(f.words); i++ {
		if compareWords(w, f.words[i]) {
			copy(f.words[i+1:], f.words[i:])
			f.words[i] = w
			break
		}
	}
	return nil
}

func (f *WordTracker) Snapshot() (raft.FSMSnapshot, error) {
	// Make sure that any future calls to f.Apply() don't change the snapshot.
	return &snapshot{cloneWords(f.words)}, nil
}

func (f *WordTracker) Restore(r io.ReadCloser) error {
	b, err := io.ReadAll(r)
	if err != nil {
		return err
	}
	words := strings.Split(string(b), "\n")
	copy(f.words[:], words)
	return nil
}

func (s *snapshot) Persist(sink raft.SnapshotSink) error {
	_, err := sink.Write([]byte(strings.Join(s.words, "\n")))
	if err != nil {
		err := sink.Cancel()
		if err != nil {
			return err
		}
		return fmt.Errorf("sink.Write(): %v", err)
	}
	return sink.Close()
}

func (s *snapshot) Release() {
}

func (r rpcInterface) AddWord(word string) (uint64, error) {
	// start := time.Now()
	start := time.Now()
	glog.Warningf("AddWord: started raft Apply at %+v", start)
	f := r.raft.Apply([]byte(word), 10*time.Microsecond)
	glog.Warningf("AddWord: Apply took: %+v", time.Since(start))
	start = time.Now()
	glog.Warningf("AddWord: block on future at %+v", start)
	//pprof.Lookup("goroutine").WriteTo(os.Stderr, 1)
	if err := f.Error(); err != nil {
		glog.Warningf("Error: couldn't block")
		return 0, errors.New("raft.Apply(): " + err.Error())
	}
	glog.Warningf("AddWord: future returned success result, took: %+v", time.Since(start))
	// elapsed := time.Since(start)
	// glog.Info("Added word: ", word, " [", elapsed.Microseconds(), " us]")
	return f.Index(), nil
}

func (r rpcInterface) GetWords() (*wordsResponse, error) {
	r.WordTracker.mtx.RLock()
	defer r.WordTracker.mtx.RUnlock()
	return &wordsResponse{
		readAtIndex: r.raft.AppliedIndex(),
		words:       cloneWords(r.WordTracker.words),
	}, nil
}
