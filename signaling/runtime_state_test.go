package main

import (
	"errors"
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
	stopped.Type = "source_stopped"
	stopped.State = "stopped"
	stopped.Stage = ""
	stopped.EndReason = "remote"
	changed, err = runtimes.apply(stopped)
	if err != nil || !changed {
		t.Fatalf("apply(stopped) = %v, %v", changed, err)
	}
	if changed, err := runtimes.apply(stopped); err != nil || changed {
		t.Fatalf("apply(duplicate stopped) = %v, %v", changed, err)
	}
	if _, err := runtimes.apply(streaming); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("late streaming error = %v", err)
	}
	mutated := stopped
	mutated.StreamName = "live/replacement"
	if _, err := runtimes.apply(mutated); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("identity mutation error = %v", err)
	}
	if snapshot := runtimes.snapshot(); len(snapshot) != 1 || snapshot[0] != stopped {
		t.Fatalf("snapshot = %+v", snapshot)
	}
}

func TestObservedRuntimeRejectsTypeFamilyChanges(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	starting := testObservedRuntime("00000000-0000-4000-8000-000000000009", "starting")
	if _, err := runtimes.apply(starting); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	changedType := starting
	changedType.Type = "publisher_connected"
	if _, err := runtimes.apply(changedType); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("active type mutation error = %v", err)
	}
	wrongTerminal := starting
	wrongTerminal.Type = "publisher_disconnected"
	wrongTerminal.State = "stopped"
	wrongTerminal.Stage = ""
	wrongTerminal.EndReason = "remote"
	if _, err := runtimes.apply(wrongTerminal); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("terminal type mutation error = %v", err)
	}
}

func TestObservedRuntimeSourceGenerationFencing(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000001"
	first := testObservedRuntime("20000000-0000-4000-8000-000000000001", "starting")
	first.SourceID = sourceID
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(first) error = %v", err)
	}
	second := testObservedRuntime("30000000-0000-4000-8000-000000000001", "starting")
	second.SourceID = sourceID
	runtimes.bindSource(sourceID, second.StreamID)
	if _, err := runtimes.apply(second); err != nil {
		t.Fatalf("apply(second) error = %v", err)
	}

	first.Type = "source_stopped"
	first.State = "stopped"
	first.Stage = ""
	first.EndReason = "requested"
	if _, err := runtimes.apply(first); err != nil {
		t.Fatalf("apply(late first stop) error = %v", err)
	}
	current, ok := runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID || current.State != "starting" {
		t.Fatalf("current runtime = %+v, %v", current, ok)
	}

	lateFirst := first
	lateFirst.Type = "source_started"
	lateFirst.State = "streaming"
	lateFirst.EndReason = ""
	if _, err := runtimes.apply(lateFirst); !errors.Is(err, errRuntimeConflict) {
		t.Fatalf("late first revival error = %v", err)
	}
	current, ok = runtimes.currentForSource(sourceID)
	if !ok || current.StreamID != second.StreamID {
		t.Fatalf("current after late revival = %+v, %v", current, ok)
	}
}

func TestObservedRuntimeRestoresOnlyCurrentSourceBinding(t *testing.T) {
	runtimes := newObservedRuntimeRegistry()
	sourceID := "10000000-0000-4000-8000-000000000002"
	previous, hadPrevious := runtimes.bindSource(sourceID, "20000000-0000-4000-8000-000000000002")
	if hadPrevious || previous != "" {
		t.Fatalf("initial binding = %q, %v", previous, hadPrevious)
	}
	previous, hadPrevious = runtimes.bindSource(sourceID, "30000000-0000-4000-8000-000000000002")
	if !hadPrevious || previous != "20000000-0000-4000-8000-000000000002" {
		t.Fatalf("replacement binding = %q, %v", previous, hadPrevious)
	}
	runtimes.restoreSourceBinding(sourceID, "20000000-0000-4000-8000-000000000002", previous, hadPrevious)
	if current := runtimes.currentBySource[sourceID]; current != "30000000-0000-4000-8000-000000000002" {
		t.Fatalf("stale restore changed binding to %q", current)
	}
	runtimes.restoreSourceBinding(sourceID, "30000000-0000-4000-8000-000000000002", previous, hadPrevious)
	if current := runtimes.currentBySource[sourceID]; current != "20000000-0000-4000-8000-000000000002" {
		t.Fatalf("current restore binding = %q", current)
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
	if len(changed) != 1 || changed[0].StreamID != oldInstance.StreamID || changed[0].State != "stopped" ||
		changed[0].Type != "runtime_error" || changed[0].EndReason != "runtime_error" || changed[0].Error != "media_server_offline" {
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

func testObservedRuntime(streamID, state string) observedRuntime {
	return observedRuntime{
		Type: "source_started", ServerID: "media-1", InstanceID: "instance-a",
		StreamID: streamID, StreamName: "live/camera", Direction: "input", Protocol: "rtsp",
		State: state, Stage: state,
	}
}
