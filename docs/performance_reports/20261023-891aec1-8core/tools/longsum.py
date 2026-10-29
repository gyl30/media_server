#!/usr/bin/env python3
import json
import sys
from pathlib import Path

WINDOW_S = 300


def snapshot(sample):
    return sample["server"] if "server" in sample else sample


def main():
    for path in sys.argv[1:]:
        run = Path(path)
        r = json.loads((run / "result.json").read_text())
        print(f"== {run.name} status={r['status']} boundary={r.get('capacity_boundary')}")
        base = snapshot(r.get("baseline", {}).get("server", r.get("baseline", {})))
        print(f"  baseline fd={base.get('fd')} threads={base.get('thread_count')} pss={base.get('pss_kib', 0) / 1024:.1f}MiB")
        churn = r.get("churn") or {}
        rounds = churn.get("rounds", [])
        bad = [x["name"] for x in rounds if not x.get("ok")]
        fds = [x.get("server_fd_before") for x in rounds]
        print(f"  churn rounds={len(rounds)} failed={churn.get('failed_rounds')} bad={bad[:5]} fd_before first={fds[:1]} last={fds[-1:]} min={min(fds) if fds else None} max={max(fds) if fds else None}")
        samples = [snapshot(s) for s in r.get("samples", []) if "server" in s]
        if samples:
            t0 = samples[0]["monotonic_ns"]
            windows = {}
            for s in samples:
                windows.setdefault(int((s["monotonic_ns"] - t0) / 1e9 // WINDOW_S), []).append(s)
            for index in sorted(windows):
                items = windows[index]
                first, last = items[0], items[-1]
                seconds = (last["monotonic_ns"] - first["monotonic_ns"]) / 1e9
                cores = (last["cpu"] - first["cpu"]) / seconds if seconds > 0 else 0
                pss = [x["pss_kib"] / 1024 for x in items]
                fd = [x["fd"] for x in items]
                drops = [x.get("udp_drops") or 0 for x in items]
                print(f"  t={index * WINDOW_S:5d}s cores={cores:.3f} pss={min(pss):.0f}-{max(pss):.0f}MiB fd={min(fd)}-{max(fd)} threads={last['thread_count']} udp_drops={drops[-1]}")
        groups = [(g.get("name"), g.get("requested"), g.get("measurement") or {}) for g in r.get("groups", [])]
        for name, requested, m in groups:
            if m.get("ready") != requested or m.get("progressing") != requested or float(m.get("viewer_realtime_min", 1)) < 0.98:
                print(f"  group {name} ready={m.get('ready')}/{requested} progressing={m.get('progressing')} rt={m.get('viewer_realtime_min')} failure={m.get('failure_class')}")
        rts = [float(m["viewer_realtime_min"]) for _, _, m in groups if m.get("viewer_realtime_min") is not None]
        if rts:
            print(f"  realtime_min over groups={min(rts):.4f}")
        cleanup = r.get("cleanup", {})
        for key in ("server_after", "server_settled"):
            s = cleanup.get(key)
            if s:
                print(f"  {key} fd={s.get('fd')} threads={s.get('thread_count')} pss={s.get('pss_kib', 0) / 1024:.1f}MiB cpu={s.get('cpu')}")
        print(f"  remaining_owned_receivers={cleanup.get('remaining_owned_receivers')} reaped={cleanup.get('all_owned_processes_reaped')} errors={[e.get('message', e)[:120] if isinstance(e, dict) else e for e in r.get('errors', [])][:3]}")


if __name__ == "__main__":
    main()
