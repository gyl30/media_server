package main

import (
	"context"
	"fmt"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"
)

type mediaTask struct {
	unit  mediaUnit
	phase int
}

type mediaWorker struct {
	index  int
	socket *net.UDPConn
	sender *udpBatchSender
	tasks  chan mediaTask
}

type mediaCounters struct {
	packets    atomic.Uint64
	bytes      atomic.Uint64
	dropped    atomic.Uint64
	sendErrors atomic.Uint64
	phaseDrops atomic.Uint64
}

type mediaEngine struct {
	cancel   context.CancelFunc
	done     chan struct{}
	stopOnce sync.Once
	err      error
	table    *sessionTable
	tableMu  sync.RWMutex
	workers  []mediaWorker
	workerWG sync.WaitGroup
	counters mediaCounters
}

func startMediaEngine(ctx context.Context, source *sharedMediaSource, bindAddress string, capacity, workerCount, phases, batchSize int, loss packetLoss) (*mediaEngine, error) {
	runContext, cancel := context.WithCancel(ctx)
	engine := &mediaEngine{
		cancel:  cancel,
		done:    make(chan struct{}),
		table:   newSessionTable(capacity, workerCount, phases),
		workers: make([]mediaWorker, workerCount),
	}
	for index := range workerCount {
		socket, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP(bindAddress)})
		if err != nil {
			cancel()
			for previous := range index {
				_ = engine.workers[previous].socket.Close()
			}
			return nil, err
		}
		engine.workers[index] = mediaWorker{
			index: index, socket: socket, sender: newUDPBatchSender(socket, batchSize, loss), tasks: make(chan mediaTask, 2),
		}
	}
	for index := range engine.workers {
		engine.workerWG.Add(1)
		go engine.runWorker(runContext, &engine.workers[index])
	}
	go engine.runScheduler(runContext, source, phases)
	return engine, nil
}

func (e *mediaEngine) add(index int, target mediaTarget) error {
	address, err := netip.ParseAddr(target.address)
	if err != nil || !address.Is4() {
		return fmt.Errorf("invalid RTP target address")
	}
	if index < 0 || index >= len(e.table.slots) {
		return fmt.Errorf("media session index is out of range")
	}
	e.tableMu.Lock()
	defer e.tableMu.Unlock()
	if !e.table.add(index, netip.AddrPortFrom(address, target.rtpPort), target.payloadType, target.ssrc) {
		return fmt.Errorf("media session is already active")
	}
	return nil
}

func (e *mediaEngine) remove(index int) bool {
	if index < 0 || index >= len(e.table.slots) {
		return false
	}
	e.tableMu.Lock()
	defer e.tableMu.Unlock()
	return e.table.remove(index)
}

func (e *mediaEngine) runScheduler(ctx context.Context, source *sharedMediaSource, phases int) {
	defer func() {
		e.cancel()
		for index := range e.workers {
			_ = e.workers[index].socket.Close()
		}
		e.workerWG.Wait()
		close(e.done)
	}()
	ticker := time.NewTicker(40 * time.Millisecond / time.Duration(phases))
	defer ticker.Stop()
	phase := 0
	var unit mediaUnit
	for {
		if phase == 0 {
			var err error
			unit, err = source.next()
			if err != nil {
				e.err = err
				return
			}
			if unit.keyframe {
				e.tableMu.Lock()
				e.table.activateWaiting()
				e.tableMu.Unlock()
			}
		}
		task := mediaTask{unit: unit, phase: phase}
		for index := range e.workers {
			select {
			case e.workers[index].tasks <- task:
			default:
				e.counters.phaseDrops.Add(1)
			}
		}
		phase = (phase + 1) % phases
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (e *mediaEngine) runWorker(ctx context.Context, worker *mediaWorker) {
	defer e.workerWG.Done()
	for {
		select {
		case <-ctx.Done():
			return
		case task := <-worker.tasks:
			e.tableMu.RLock()
			packets, bytesSent, dropped, err := worker.sender.send(e.table, e.table.active(worker.index, task.phase), task.unit)
			e.tableMu.RUnlock()
			e.counters.packets.Add(uint64(packets))
			e.counters.bytes.Add(uint64(bytesSent))
			if err != nil && ctx.Err() != nil {
				continue
			}
			e.counters.dropped.Add(uint64(dropped))
			if err != nil {
				e.counters.sendErrors.Add(1)
			}
		}
	}
}

func (e *mediaEngine) stop() error {
	e.stopOnce.Do(e.cancel)
	<-e.done
	return e.err
}
