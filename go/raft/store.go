package main

import (
	"bytes"
	"encoding/binary"
	"github.com/golang/glog"
	"sync"
	"time"

	"github.com/hashicorp/go-msgpack/codec"
	"github.com/hashicorp/raft"
)

var ErrKeyNotFound error

// Decode reverses the encode operation on a byte slice input
func decodeMsgPack(buf []byte, out interface{}) error {
	r := bytes.NewBuffer(buf)
	hd := codec.MsgpackHandle{}
	dec := codec.NewDecoder(r, &hd)
	return dec.Decode(out)
}

// Encode writes an encoded object to a new bytes buffer
func encodeMsgPack(in interface{}) (*bytes.Buffer, error) {
	buf := bytes.NewBuffer(nil)
	hd := codec.MsgpackHandle{}
	enc := codec.NewEncoder(buf, &hd)
	err := enc.Encode(in)
	return buf, err
}

// Converts bytes to an integer
func bytesToUint64(b []byte) uint64 {
	return binary.BigEndian.Uint64(b)
}

// Converts "a" uint to a byte slice
func uint64ToBytes(u uint64) []byte {
	buf := make([]byte, 8)
	binary.BigEndian.PutUint64(buf, u)
	return buf
}

// BuffStore Make an in-memory buffer store implementation of LogStore and StableStore for raft.
type BuffStore struct {
	entries      map[uint64][]byte
	entriesMutex sync.RWMutex

	indexQueue []uint64
	indexMutex sync.RWMutex

	maxEntries int
}

func NewBuffStore(maxEntries int) *BuffStore {
	return &BuffStore{
		entries:      make(map[uint64][]byte),
		entriesMutex: sync.RWMutex{},
		indexQueue:   make([]uint64, 0),
		indexMutex:   sync.RWMutex{},
		maxEntries:   maxEntries,
	}
}

func (s *BuffStore) FirstIndex() (uint64, error) {
	if len(s.indexQueue) == 0 {
		return 0, nil
	}

	s.indexMutex.RLock()
	defer s.indexMutex.RUnlock()
	ret := s.indexQueue[0]
	return ret, nil
}

func (s *BuffStore) LastIndex() (uint64, error) {
	if len(s.indexQueue) == 0 {
		return 0, nil
	}

	s.indexMutex.RLock()
	defer s.indexMutex.RUnlock()
	ret := s.indexQueue[len(s.indexQueue)-1]
	return ret, nil
}

func (s *BuffStore) GetLog(index uint64, log *raft.Log) error {
	s.entriesMutex.RLock()
	defer s.entriesMutex.RUnlock()
	if val, ok := s.entries[index]; ok {
		return decodeMsgPack(val, log)
	}
	return raft.ErrLogNotFound
}

func (s *BuffStore) StoreLog(log *raft.Log) error {
	// Encode the log.
	val, err := encodeMsgPack(log)
	if err != nil {
		return err
	}

	// Capture locks.
	s.entriesMutex.Lock()
	defer s.entriesMutex.Unlock()
	s.indexMutex.Lock()
	defer s.indexMutex.Unlock()

	s.entries[log.Index] = val.Bytes()
	s.indexQueue = append(s.indexQueue, log.Index)

	// If we have reached the max number of entries, remove the oldest entry.
	if len(s.indexQueue) > int(s.maxEntries) {
		delete(s.entries, s.indexQueue[0])
		s.indexQueue = s.indexQueue[1:]
	}

	return nil
}

func (s *BuffStore) StoreLogs(logs []*raft.Log) error {
	start := time.Now()
	glog.Warningf("StoreLogs: started at %+v", start)
	for _, log := range logs {
		if err := s.StoreLog(log); err != nil {
			return err
		}
	}
	glog.Warningf("StoreLogs: took %+v", time.Since(start))
	return nil
}

func (s *BuffStore) DeleteRange(min, max uint64) error {
	s.entriesMutex.Lock()
	defer s.entriesMutex.Unlock()
	s.indexMutex.Lock()
	defer s.indexMutex.Unlock()

	for i := min; i <= max; i++ {
		delete(s.entries, i)
		// Remove the index from the queue.
		for j, index := range s.indexQueue {
			if index == i {
				s.indexQueue = append(s.indexQueue[:j], s.indexQueue[j+1:]...)
				break
			}
		}
	}

	return nil
}

func (s *BuffStore) Set(key []byte, val []byte) error {
	// Convert the key to uint64.
	keyInt := bytesToUint64(key)

	s.entriesMutex.Lock()
	defer s.entriesMutex.Unlock()
	s.entries[keyInt] = val
	return nil
}

func (s *BuffStore) Get(key []byte) ([]byte, error) {
	// Convert the key to uint64.
	keyInt := bytesToUint64(key)

	s.entriesMutex.RLock()
	defer s.entriesMutex.RUnlock()
	if val, ok := s.entries[keyInt]; ok {
		return append([]byte(nil), val...), nil
	}

	// Return an empty byte slice if the key is not found.
	return nil, ErrKeyNotFound
}

func (s *BuffStore) SetUint64(key []byte, val uint64) error {
	// Convert val to a byte array.
	return s.Set(key, uint64ToBytes(val))
}

func (s *BuffStore) GetUint64(key []byte) (uint64, error) {
	valBytes, err := s.Get(key)
	if err != nil || valBytes == nil {
		return 0, err
	}
	return bytesToUint64(valBytes), nil
}
