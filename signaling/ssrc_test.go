package main

import "testing"

func TestSSRCAllocatorUsesGBDomainAndReleases(t *testing.T) {
	allocator, err := newSSRCAllocator("3402000000")
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	first, err := allocator.acquire()
	if err != nil {
		t.Fatalf("acquire() error = %v", err)
	}
	second, err := allocator.acquire()
	if err != nil {
		t.Fatalf("acquire() error = %v", err)
	}
	if first != 200000001 || second != 200000002 {
		t.Fatalf("SSRCs = %010d, %010d", first, second)
	}
	allocator.release(first)
	if allocator.activeCount() != 1 {
		t.Fatalf("active count = %d", allocator.activeCount())
	}
}

func TestSSRCAllocatorRejectsInvalidDomain(t *testing.T) {
	for _, domain := range []string{"", "340200", "340200000x"} {
		if _, err := newSSRCAllocator(domain); err == nil {
			t.Fatalf("newSSRCAllocator(%q) succeeded", domain)
		}
	}
}
