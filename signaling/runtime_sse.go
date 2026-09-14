package main

import (
	"encoding/json"
	"io"
	"net/http"
	"sync"
)

const runtimeEventSubscriberCapacity = 1

type runtimeEventHub struct {
	mu          sync.Mutex
	subscribers map[chan observedRuntime]struct{}
	closed      bool
}

func newRuntimeEventHub() *runtimeEventHub {
	return &runtimeEventHub{subscribers: make(map[chan observedRuntime]struct{})}
}

func (h *runtimeEventHub) subscribe() (chan observedRuntime, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		return nil, false
	}
	events := make(chan observedRuntime, runtimeEventSubscriberCapacity)
	h.subscribers[events] = struct{}{}
	return events, true
}

func (h *runtimeEventHub) unsubscribe(events chan observedRuntime) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, subscribed := h.subscribers[events]; !subscribed {
		return
	}
	delete(h.subscribers, events)
	close(events)
}

func (h *runtimeEventHub) publish(event observedRuntime) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		return
	}
	for events := range h.subscribers {
		select {
		case events <- event:
		default:
			delete(h.subscribers, events)
			close(events)
		}
	}
}

func (h *runtimeEventHub) close() {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		return
	}
	h.closed = true
	for events := range h.subscribers {
		delete(h.subscribers, events)
		close(events)
	}
}

func (s *infrastructureServer) handleRuntimeEvents(writer http.ResponseWriter, request *http.Request) {
	flusher, ok := writer.(http.Flusher)
	if !ok {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	events, ok := s.runtimeEvents.subscribe()
	if !ok {
		writeHTTPError(writer, http.StatusServiceUnavailable, "server_shutdown")
		return
	}
	defer s.runtimeEvents.unsubscribe(events)

	writer.Header().Set("Content-Type", "text/event-stream")
	writer.Header().Set("Cache-Control", "no-cache")
	writer.WriteHeader(http.StatusOK)
	if _, err := io.WriteString(writer, ": connected\n\n"); err != nil {
		return
	}
	flusher.Flush()

	for {
		select {
		case event, open := <-events:
			if !open {
				return
			}
			body, err := json.Marshal(event)
			if err != nil {
				return
			}
			if _, err = io.WriteString(writer, "event: runtime\ndata: "); err != nil {
				return
			}
			if _, err = writer.Write(body); err != nil {
				return
			}
			if _, err = io.WriteString(writer, "\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case <-request.Context().Done():
			return
		}
	}
}
