package main

import (
	"net/netip"
	"testing"
	"unsafe"
)

func TestSessionSlotFitsResourceBudget(t *testing.T) {
	size := unsafe.Sizeof(sessionSlot{})
	t.Logf("sessionSlot size = %d", size)
	if size > 128 {
		t.Fatalf("sessionSlot size = %d, want at most 128", size)
	}
}

func TestSessionTableWaitsForKeyframeAndRemovesDensely(t *testing.T) {
	table := newSessionTable(6, 2, 4)
	for index := range 4 {
		if !table.add(index, netip.MustParseAddrPort("127.0.0.1:40000"), 96, uint32(200000001+index)) {
			t.Fatalf("add(%d) failed", index)
		}
	}
	if table.activeCount() != 0 || table.waitingCount() != 4 {
		t.Fatalf("before keyframe active=%d waiting=%d", table.activeCount(), table.waitingCount())
	}
	table.activateWaiting()
	if table.activeCount() != 4 || table.waitingCount() != 0 {
		t.Fatalf("after keyframe active=%d waiting=%d", table.activeCount(), table.waitingCount())
	}
	if got := table.active(0, 0); len(got) != 1 || got[0] != 0 {
		t.Fatalf("worker 0 phase 0 = %v", got)
	}
	if got := table.active(1, 3); len(got) != 1 || got[0] != 3 {
		t.Fatalf("worker 1 phase 3 = %v", got)
	}
	if !table.remove(1) || table.remove(1) {
		t.Fatal("remove result is wrong")
	}
	if table.activeCount() != 3 || len(table.active(1, 1)) != 0 {
		t.Fatalf("after remove active=%d bucket=%v", table.activeCount(), table.active(1, 1))
	}
	if !table.add(4, netip.MustParseAddrPort("127.0.0.1:40004"), 96, 200000005) || !table.remove(4) {
		t.Fatal("waiting session add/remove failed")
	}
	if table.waitingCount() != 0 {
		t.Fatalf("waiting count = %d", table.waitingCount())
	}
}
