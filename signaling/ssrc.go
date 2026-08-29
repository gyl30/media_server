package main

import (
	"fmt"
	"strconv"
	"sync"
)

type ssrcAllocator struct {
	mu     sync.Mutex
	prefix uint32
	next   uint32
	active map[uint32]struct{}
}

func newSSRCAllocator(domain string) (*ssrcAllocator, error) {
	if !validDigits(domain, 10) {
		return nil, fmt.Errorf("invalid SIP domain")
	}
	prefix, err := strconv.ParseUint(domain[3:8], 10, 32)
	if err != nil {
		return nil, err
	}
	return &ssrcAllocator{prefix: uint32(prefix) * 10000, next: 1, active: make(map[uint32]struct{})}, nil
}

func (a *ssrcAllocator) acquire() (uint32, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	for range 9999 {
		candidate := a.prefix + a.next
		a.next++
		if a.next == 10000 {
			a.next = 1
		}
		if _, exists := a.active[candidate]; exists {
			continue
		}
		a.active[candidate] = struct{}{}
		return candidate, nil
	}
	return 0, fmt.Errorf("no GB28181 SSRC available")
}

func (a *ssrcAllocator) release(ssrc uint32) {
	a.mu.Lock()
	defer a.mu.Unlock()
	delete(a.active, ssrc)
}

func (a *ssrcAllocator) activeCount() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.active)
}
