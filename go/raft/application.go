package main

import (
	"errors"
	"fmt"
	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/golang/glog"
	"github.com/hashicorp/raft"
	"io"
	"strings"
	"sync"
	"time"
)

// WordTracker keeps track of the three longest words it ever saw.
type WordTracker struct {
	mtx   sync.RWMutex
	words [3]string
}

type rpcInterface struct {
	WordTracker      *WordTracker
	raft             *raft.Raft
	histogram        *hdrhistogram.Histogram
	lastRecordedTime time.Time
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

func (r *rpcInterface) AddWord(word []byte) (uint64, error) {
	start := time.Now()
	//glog.Warningf("AddWord[%s]: started raft Apply at %+v", word, start)
	f := r.raft.Apply(word, 0) // 10*time.Microsecond)
	//glog.Warningf("AddWord[%s]: Apply took: %+v returned future: %+v", word, time.Since(start), f)
	//start = time.Now()
	//glog.Warningf("AddWord[%s]: block on future at %+v", word, start)
	if err := f.Error(); err != nil {
		glog.Warningf("Error: couldn't block")
		return 0, errors.New("raft.Apply(): " + err.Error())
	}
	err := r.histogram.RecordValue(time.Since(start).Microseconds())
	if err != nil {
		glog.Errorf("Failed to record to histogram")
	}
	if time.Since(r.lastRecordedTime) > 1*time.Second {
		percentileValues := r.histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
		glog.Warningf("Future wait time: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us RPS: %d ",

			float64(percentileValues[50.0]), float64(percentileValues[95.0]),
			float64(percentileValues[99.0]), float64(percentileValues[99.9]), r.histogram.TotalCount())
		r.histogram.Reset()
		r.lastRecordedTime = time.Now()
		//glog.Warningf("LatRecordedTime updated: %v", r.lastRecordedTime)
	}
	//glog.Warningf("AddWord: future returned success result, took: %+v", time.Since(start))
	return f.Index(), nil
}

func (r *rpcInterface) GetWords() (*wordsResponse, error) {
	r.WordTracker.mtx.RLock()
	defer r.WordTracker.mtx.RUnlock()
	return &wordsResponse{
		readAtIndex: r.raft.AppliedIndex(),
		words:       cloneWords(r.WordTracker.words),
	}, nil
}
