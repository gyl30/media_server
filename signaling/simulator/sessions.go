package main

import (
	"net"
	"net/netip"
)

type sessionState uint8

const (
	sessionIdle sessionState = iota
	sessionWaiting
	sessionActive
)

type sessionSlot struct {
	destination netip.AddrPort
	address     *net.UDPAddr
	rtp         rtpSession
	payloadType uint8
	worker      int
	phase       int
	position    int
	state       sessionState
}

type sessionTable struct {
	slots         []sessionSlot
	waiting       []int
	activeBuckets [][][]int
	activeTotal   int
	workers       int
	phases        int
}

func newSessionTable(capacity, workers, phases int) *sessionTable {
	buckets := make([][][]int, workers)
	for worker := range workers {
		buckets[worker] = make([][]int, phases)
	}
	return &sessionTable{
		slots:         make([]sessionSlot, capacity),
		waiting:       make([]int, 0, capacity),
		activeBuckets: buckets,
		workers:       workers,
		phases:        phases,
	}
}

func (t *sessionTable) add(index int, destination netip.AddrPort, payloadType uint8, ssrc uint32) bool {
	slot := &t.slots[index]
	if slot.state != sessionIdle {
		return false
	}
	worker, phase := assignMedia(index, t.workers, t.phases)
	*slot = sessionSlot{
		destination: destination,
		address:     net.UDPAddrFromAddrPort(destination),
		rtp:         rtpSession{ssrc: ssrc},
		payloadType: payloadType,
		worker:      worker,
		phase:       phase,
		position:    len(t.waiting),
		state:       sessionWaiting,
	}
	t.waiting = append(t.waiting, index)
	return true
}

func (t *sessionTable) activateWaiting() {
	for _, index := range t.waiting {
		slot := &t.slots[index]
		bucket := t.activeBuckets[slot.worker][slot.phase]
		slot.position = len(bucket)
		slot.state = sessionActive
		t.activeBuckets[slot.worker][slot.phase] = append(bucket, index)
		t.activeTotal++
	}
	t.waiting = t.waiting[:0]
}

func (t *sessionTable) remove(index int) bool {
	slot := &t.slots[index]
	switch slot.state {
	case sessionWaiting:
		moved := t.waiting[len(t.waiting)-1]
		t.waiting[slot.position] = moved
		t.slots[moved].position = slot.position
		t.waiting = t.waiting[:len(t.waiting)-1]
	case sessionActive:
		bucket := t.activeBuckets[slot.worker][slot.phase]
		moved := bucket[len(bucket)-1]
		bucket[slot.position] = moved
		t.slots[moved].position = slot.position
		t.activeBuckets[slot.worker][slot.phase] = bucket[:len(bucket)-1]
		t.activeTotal--
	default:
		return false
	}
	*slot = sessionSlot{}
	return true
}

func (t *sessionTable) active(worker, phase int) []int {
	return t.activeBuckets[worker][phase]
}

func (t *sessionTable) activeCount() int {
	return t.activeTotal
}

func (t *sessionTable) waitingCount() int {
	return len(t.waiting)
}
