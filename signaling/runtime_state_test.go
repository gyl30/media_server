package main

import (
	"errors"
	"fmt"
	"testing"
)

func TestObservedRuntimeTransitionsAndIdentity(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	starting := testObservedRuntime("00000000-0000-4000-8000-000000000001", "starting")
	changed, err := runtimes.apply(starting)
	if err != nil || !changed {
		t.Fatalf("apply(starting) = %v, %v", changed, err)
	}
	changed, err = runtimes.apply(starting)
	if err != nil || changed {
		t.Fatalf("apply(duplicate) = %v, %v", changed, err)
	}
	streaming := starting
	streaming.State = "streaming"
	streaming.Stage = "streaming"
	changed, err = runtimes.apply(streaming)
	if err != nil || !changed {
		t.Fatalf("apply(streaming) = %v, %v", changed, err)
	}
	stopped := streaming
	stopped.State = "stopped"
	stopped.Stage = ""
	changed, err = runtimes.apply(stopped)
	if err != nil || !changed {
		t.Fatalf("apply(stopped) = %v, %v", changed, err)
	}
	if changed, err := runtimes.apply(stopped); err != nil || changed {
		t.Fatalf("apply(duplicate stopped) = %v, %v", changed, err)
	}
	if changed, err := runtimes.apply(streaming); err != nil || !changed {
		t.Fatalf("apply(late streaming) = %v, %v", changed, err)
	}
	mutated := stopped
	mutated.StreamName = "live/replacement"
	if _, err := runtimes.apply(mutated); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("identity mutation error = %v", err)
	}
	if snapshot := runtimes.snapshot(); len(snapshot) != 1 || snapshot[0] != streaming {
		t.Fatalf("snapshot = %+v", snapshot)
	}
}

func TestObservedRuntimeFactsDoNotChangeLifecycleState(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	starting := testObservedRuntime("00000000-0000-4000-8000-000000000010", "starting")
	if _, err := runtimes.apply(starting); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	stopped := starting
	stopped.State = "stopped"
	stopped.Stage = ""
	if _, err := runtimes.apply(stopped); err != nil {
		t.Fatalf("apply(stopped) error = %v", err)
	}

	var changes []observedRuntime
	runtimes.setOnChange(func(event observedRuntime) { changes = append(changes, event) })
	fact := starting
	fact.State = "runtime_error"
	fact.Stage = "transport"
	fact.Error = "connection_failed"
	if changed, err := runtimes.apply(fact); err != nil || !changed {
		t.Fatalf("apply(fact) = %v, %v", changed, err)
	}
	if changed, err := runtimes.apply(fact); err != nil || !changed {
		t.Fatalf("apply(repeated fact) = %v, %v", changed, err)
	}

	snapshot := runtimes.snapshot()
	if len(snapshot) != 1 || snapshot[0] != stopped {
		t.Fatalf("snapshot = %+v", snapshot)
	}
	if len(changes) != 2 || changes[0] != fact || changes[1] != fact {
		t.Fatalf("changes = %+v", changes)
	}
}

func TestObservedRuntimeRejectsKindChanges(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	starting := testObservedRuntime("00000000-0000-4000-8000-000000000009", "starting")
	if _, err := runtimes.apply(starting); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	changedKind := starting
	changedKind.Kind = "publisher"
	if _, err := runtimes.apply(changedKind); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("active kind mutation error = %v", err)
	}
	wrongTerminal := starting
	wrongTerminal.Kind = "publisher"
	wrongTerminal.State = "stopped"
	wrongTerminal.Stage = ""
	if _, err := runtimes.apply(wrongTerminal); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("terminal kind mutation error = %v", err)
	}
}

func TestObservedRuntimeSourceGenerationFencing(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000001"
	first := testObservedRuntime("20000000-0000-4000-8000-000000000001", "starting")
	first.SourceID = sourceID
	runtimes.bindSource(sourceID, first.StreamID)
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(first) error = %v", err)
	}
	second := testObservedRuntime("30000000-0000-4000-8000-000000000001", "starting")
	second.SourceID = sourceID
	runtimes.bindSource(sourceID, second.StreamID)
	if _, err := runtimes.apply(second); err != nil {
		t.Fatalf("apply(second) error = %v", err)
	}
	lateFirstStreaming := first
	lateFirstStreaming.State = "streaming"
	lateFirstStreaming.Stage = "streaming"
	if changed, err := runtimes.apply(lateFirstStreaming); err != nil || !changed {
		t.Fatalf("apply(late first streaming) = %v, %v", changed, err)
	}

	first.State = "stopped"
	first.Stage = ""
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(late first stop) error = %v", err)
	}
	current, ok := runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID || current.State != "starting" {
		t.Fatalf("current runtime = %+v, %v", current, ok)
	}

	lateFirst := first
	lateFirst.State = "streaming"
	if changed, err := runtimes.apply(lateFirst); err != nil || !changed {
		t.Fatalf("apply(late first revival) = %v, %v", changed, err)
	}
	current, ok = runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID {
		t.Fatalf("current after late revival = %+v, %v", current, ok)
	}
	lateFirst.State = "stopped"
	lateFirst.Stage = ""
	if changed, err := runtimes.apply(lateFirst); err != nil || !changed {
		t.Fatalf("apply(late first stop) = %v, %v", changed, err)
	}

	for index := range 501 {
		if _, err := runtimes.apply(testHistoricalStoppedRuntime(1000 + index)); err != nil {
			t.Fatalf("apply(history %d) error = %v", index, err)
		}
	}
	if _, exists := runtimes.byStreamID[first.StreamID]; exists {
		t.Fatal("replaced generation was not subject to history retention")
	}
	if _, err := runtimes.apply(lateFirst); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("evicted generation revival error = %v", err)
	}
	current, ok = runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID {
		t.Fatalf("current after evicted revival = %+v, %v", current, ok)
	}
}

func TestObservedRuntimeRestoresOnlyCurrentSourceBinding(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000002"
	previous, hadPrevious, previousRuntime := runtimes.bindSource(sourceID, "20000000-0000-4000-8000-000000000002")
	if hadPrevious || previous != "" {
		t.Fatalf("initial binding = %q, %v", previous, hadPrevious)
	}
	previous, hadPrevious, previousRuntime = runtimes.bindSource(sourceID, "30000000-0000-4000-8000-000000000002")
	if !hadPrevious || previous != "20000000-0000-4000-8000-000000000002" {
		t.Fatalf("replacement binding = %q, %v", previous, hadPrevious)
	}
	runtimes.restoreSourceBinding(
		sourceID, "20000000-0000-4000-8000-000000000002", previous, hadPrevious, previousRuntime)
	if current := runtimes.currentBySource[sourceID]; current != "30000000-0000-4000-8000-000000000002" {
		t.Fatalf("stale restore changed binding to %q", current)
	}
	runtimes.restoreSourceBinding(
		sourceID, "30000000-0000-4000-8000-000000000002", previous, hadPrevious, previousRuntime)
	if current := runtimes.currentBySource[sourceID]; current != "20000000-0000-4000-8000-000000000002" {
		t.Fatalf("current restore binding = %q", current)
	}
}

func TestObservedRuntimeRestoresEvictedPreviousSourceRuntime(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000006"
	previous := testHistoricalStoppedRuntime(7000)
	previous.Kind = "source"
	previous.Protocol = "rtsp"
	previous.SourceID = sourceID
	runtimes.bindSource(sourceID, previous.StreamID)
	if _, err := runtimes.apply(previous); err != nil {
		t.Fatalf("apply(previous) error = %v", err)
	}
	replacementID := testHistoricalStoppedRuntime(7001).StreamID
	previousID, hadPrevious, previousRuntime := runtimes.bindSource(sourceID, replacementID)
	if !hadPrevious || previousID != previous.StreamID {
		t.Fatalf("previous binding = %q, %v", previousID, hadPrevious)
	}
	replacement := previous
	replacement.StreamID = replacementID
	if _, err := runtimes.apply(replacement); err != nil {
		t.Fatalf("apply(replacement) error = %v", err)
	}
	for index := range maxRecentStoppedRuntimes {
		if _, err := runtimes.apply(testHistoricalStoppedRuntime(8000 + index)); err != nil {
			t.Fatalf("apply(history %d) error = %v", index, err)
		}
	}
	if _, exists := runtimes.byStreamID[previous.StreamID]; exists {
		t.Fatal("previous runtime was not evicted while replacement was current")
	}
	runtimes.restoreSourceBinding(sourceID, replacementID, previousID, hadPrevious, previousRuntime)
	if current, exists := runtimes.currentForSource(sourceID); !exists || current != previous {
		t.Fatalf("restored runtime = %+v, %v", current, exists)
	}
	if stored, exists := runtimes.byStreamID[replacementID]; !exists || stored != replacement {
		t.Fatalf("failed replacement history = %+v, %v", stored, exists)
	}
}

func TestObservedRuntimeMarksExactMediaServerOffline(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	oldInstance := testObservedRuntime("40000000-0000-4000-8000-000000000001", "streaming")
	newInstance := testObservedRuntime("50000000-0000-4000-8000-000000000001", "streaming")
	newInstance.InstanceID = "instance-b"
	if _, err := runtimes.apply(oldInstance); err != nil {
		t.Fatalf("apply(old) error = %v", err)
	}
	if _, err := runtimes.apply(newInstance); err != nil {
		t.Fatalf("apply(new) error = %v", err)
	}
	changed := runtimes.mediaServerOffline("media-1", "instance-a")
	if len(changed) != 1 || changed[0].StreamID != oldInstance.StreamID || changed[0].Kind != oldInstance.Kind ||
		changed[0].State != "stopped" || changed[0].Error != "" {
		t.Fatalf("offline changes = %+v", changed)
	}
	snapshot := runtimes.snapshot()
	if len(snapshot) != 2 || snapshot[0].State != "stopped" || snapshot[1].State != "streaming" {
		t.Fatalf("offline snapshot = %+v", snapshot)
	}
	if changed := runtimes.mediaServerOffline("media-1", "instance-a"); len(changed) != 0 {
		t.Fatalf("repeated offline changes = %+v", changed)
	}
}

func TestObservedRuntimeAcknowledgesRequestedStopByGeneration(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000003"
	first := testObservedRuntime("20000000-0000-4000-8000-000000000003", "streaming")
	first.SourceID = sourceID
	runtimes.bindSource(sourceID, first.StreamID)
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(first) error = %v", err)
	}
	second := testObservedRuntime("30000000-0000-4000-8000-000000000003", "streaming")
	second.SourceID = sourceID
	runtimes.bindSource(sourceID, second.StreamID)
	if _, err := runtimes.apply(second); err != nil {
		t.Fatalf("apply(second) error = %v", err)
	}
	server := mediaServerInstance{serverID: first.ServerID, instanceID: first.InstanceID}
	if changed, err := runtimes.acknowledgeSourceStopped(
		server, first.StreamID, first.StreamName, first.SourceID, first.Protocol); err != nil || !changed {
		t.Fatalf("acknowledge(first) = %v, %v", changed, err)
	}
	current, ok := runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID || current.State != "streaming" {
		t.Fatalf("current runtime = %+v, %v", current, ok)
	}

	terminal := second
	terminal.State = "stopped"
	terminal.Stage = ""
	if _, err := runtimes.apply(terminal); err != nil {
		t.Fatalf("apply(terminal) error = %v", err)
	}
	if changed, err := runtimes.acknowledgeSourceStopped(
		server, second.StreamID, second.StreamName, second.SourceID, second.Protocol); err != nil || changed {
		t.Fatalf("acknowledge(existing terminal) = %v, %v", changed, err)
	}
	current, ok = runtimes.currentForSource(sourceID)
	if !ok || current != terminal {
		t.Fatalf("terminal reason was overwritten: %+v, %v", current, ok)
	}

	thirdID := "40000000-0000-4000-8000-000000000003"
	runtimes.bindSource(sourceID, thirdID)
	if changed, err := runtimes.acknowledgeSourceStopped(
		server, thirdID, first.StreamName, sourceID, first.Protocol); err != nil || !changed {
		t.Fatalf("acknowledge(unseen) = %v, %v", changed, err)
	}
	lateStarting := first
	lateStarting.StreamID = thirdID
	if changed, err := runtimes.apply(lateStarting); err != nil || !changed {
		t.Fatalf("apply(late starting) = %v, %v", changed, err)
	}
}

func TestObservedRuntimeBoundsHistoricalStopped(t *testing.T) {
	const retentionLimit = 500
	runtimes := newObservedRuntimeRegistry()
	for index := range 1001 {
		event := testHistoricalStoppedRuntime(index)
		if changed, err := runtimes.apply(event); err != nil || !changed {
			t.Fatalf("apply(%d) = %v, %v", index, changed, err)
		}
	}
	if snapshot := runtimes.snapshot(); len(snapshot) != retentionLimit {
		t.Fatalf("snapshot size = %d", len(snapshot))
	}
	if _, exists := runtimes.byStreamID[testHistoricalStoppedRuntime(500).StreamID]; exists {
		t.Fatal("old historical runtime was retained")
	}
	if newest := testHistoricalStoppedRuntime(1000); runtimes.byStreamID[newest.StreamID] != newest {
		t.Fatalf("newest historical runtime = %+v", runtimes.byStreamID[newest.StreamID])
	}
}

func TestObservedRuntimeRetentionPreservesActiveAndCurrentSource(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	active := testHistoricalStoppedRuntime(2000)
	active.State = "streaming"
	active.Stage = "streaming"
	if _, err := runtimes.apply(active); err != nil {
		t.Fatalf("apply(active) error = %v", err)
	}
	pinned := testHistoricalStoppedRuntime(2001)
	pinned.Kind = "source"
	pinned.Protocol = "rtsp"
	pinned.SourceID = "10000000-0000-4000-8000-000000000004"
	runtimes.bindSource(pinned.SourceID, pinned.StreamID)
	if _, err := runtimes.apply(pinned); err != nil {
		t.Fatalf("apply(pinned) error = %v", err)
	}
	for index := range 1001 {
		if _, err := runtimes.apply(testHistoricalStoppedRuntime(3000 + index)); err != nil {
			t.Fatalf("apply(history %d) error = %v", index, err)
		}
	}
	if stored, exists := runtimes.byStreamID[active.StreamID]; !exists || stored != active {
		t.Fatalf("active runtime = %+v, %v", stored, exists)
	}
	if current, exists := runtimes.currentForSource(pinned.SourceID); !exists || current != pinned {
		t.Fatalf("pinned runtime = %+v, %v", current, exists)
	}
	if snapshot := runtimes.snapshot(); len(snapshot) != 502 {
		t.Fatalf("snapshot size = %d", len(snapshot))
	}
}

func TestObservedRuntimeUnbindsOnlyExpectedSourceGeneration(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000005"
	first := testHistoricalStoppedRuntime(5001)
	first.Kind = "source"
	first.Protocol = "rtsp"
	first.SourceID = sourceID
	runtimes.bindSource(sourceID, first.StreamID)
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(first) error = %v", err)
	}
	second := first
	second.StreamID = testHistoricalStoppedRuntime(5002).StreamID
	runtimes.bindSource(sourceID, second.StreamID)
	if _, err := runtimes.apply(second); err != nil {
		t.Fatalf("apply(second) error = %v", err)
	}
	if runtimes.unbindSource(sourceID, first.StreamID) {
		t.Fatal("stale generation unbound replacement")
	}
	if current, exists := runtimes.currentForSource(sourceID); !exists || current != second {
		t.Fatalf("current after stale unbind = %+v, %v", current, exists)
	}
	if !runtimes.unbindSource(sourceID, second.StreamID) {
		t.Fatal("current generation was not unbound")
	}
	if _, exists := runtimes.currentBySource[sourceID]; exists {
		t.Fatal("source binding was retained")
	}
	if stored, exists := runtimes.byStreamID[second.StreamID]; !exists || stored != second {
		t.Fatalf("unbound stopped runtime = %+v, %v", stored, exists)
	}
	if changed, err := runtimes.apply(second); err != nil || changed {
		t.Fatalf("replay unbound stopped runtime = %v, %v", changed, err)
	}
	lateStreaming := second
	lateStreaming.State = "streaming"
	lateStreaming.Stage = "streaming"
	if changed, err := runtimes.apply(lateStreaming); err != nil || !changed {
		t.Fatalf("apply(late unbound streaming) = %v, %v", changed, err)
	}
	if _, bound := runtimes.currentBySource[sourceID]; bound {
		t.Fatal("late unbound streaming rebound source")
	}
}

func TestObservedRuntimeDoesNotRebindUnboundSource(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000007"
	streamID := "20000000-0000-4000-8000-000000000007"
	starting := testObservedRuntime(streamID, "starting")
	starting.SourceID = sourceID
	runtimes.bindSource(sourceID, streamID)
	if !runtimes.unbindSource(sourceID, streamID) {
		t.Fatal("unobserved source was not unbound")
	}
	if _, err := runtimes.apply(starting); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("late first event error = %v", err)
	}
	if _, exists := runtimes.byStreamID[streamID]; exists {
		t.Fatal("late first event created observed runtime")
	}
	if _, bound := runtimes.currentBySource[sourceID]; bound {
		t.Fatal("late first event rebound source")
	}
}

func TestObservedRuntimeDuplicateStoppedDoesNotRepeatRetention(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	for index := range maxRecentStoppedRuntimes {
		if changed, err := runtimes.apply(testHistoricalStoppedRuntime(6000 + index)); err != nil || !changed {
			t.Fatalf("apply(stopped %d) = %v, %v", index, changed, err)
		}
	}
	stopped := testHistoricalStoppedRuntime(6000)
	if changed, err := runtimes.apply(stopped); err != nil || changed {
		t.Fatalf("apply(duplicate) = %v, %v", changed, err)
	}
	if len(runtimes.recentStopped) != maxRecentStoppedRuntimes || runtimes.recentStopped[0] != stopped.StreamID {
		t.Fatalf("recent stopped = %v", runtimes.recentStopped)
	}
	newest := testHistoricalStoppedRuntime(6000 + maxRecentStoppedRuntimes)
	if changed, err := runtimes.apply(newest); err != nil || !changed {
		t.Fatalf("apply(newest) = %v, %v", changed, err)
	}
	if _, exists := runtimes.byStreamID[stopped.StreamID]; exists {
		t.Fatal("duplicate stopped moved oldest runtime in retention order")
	}
	if _, exists := runtimes.byStreamID[testHistoricalStoppedRuntime(6001).StreamID]; !exists {
		t.Fatal("retention evicted the wrong runtime")
	}
}

func testObservedRuntime(streamID, state string) observedRuntime {
	return observedRuntime{
		Kind: "source", ServerID: "media-1", InstanceID: "instance-a",
		StreamID: streamID, StreamName: "live/camera", Protocol: "rtsp",
		State: state, Stage: state,
	}
}

func testHistoricalStoppedRuntime(index int) observedRuntime {
	return observedRuntime{
		Kind: "publisher", ServerID: "media-1", InstanceID: "instance-a",
		StreamID: fmt.Sprintf("00000000-0000-4000-8000-%012d", index), StreamName: "live/camera",
		Protocol: "rtmp", State: "stopped",
	}
}
