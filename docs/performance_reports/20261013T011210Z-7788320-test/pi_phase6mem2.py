#!/usr/bin/env python3
"""P0-M：WHEP 内存稳定性与清理后回落。

在 namespace 内、运行包根目录执行。自己拥有 signaling、media_server、perf_publish 与采样器，
结束时只停止自己启动的进程。

关键点是让 DUT 跨 viewer 关闭而存活：播放用 perf_run 的 existing 模式（明确不停止外部服务），
因此可以在"全部 viewer 关闭"之后继续观察同一进程的内存。

阶段：idle -> 发布 -> 周期 1（建立/稳态/关闭后观察）-> 周期 2（可选，用于泄漏对照）-> 停止发布 -> 结束。
"""
import json
import os
import pathlib
import signal
import subprocess
import sys
import time


def _terminate(signum, frame):
    """让 SIGTERM 走正常异常路径，保证 finally 里的清理与落盘执行。"""
    raise SystemExit(128 + signum)

OUT = pathlib.Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
VIEWERS = int(sys.argv[2])
STEADY = int(sys.argv[3])
TEARDOWN = int(sys.argv[4])
CYCLES = int(sys.argv[5])  # 1 或 2；第 2 轮用于对照重复建立后的内存水位
IDLE_OBS = 60
PUB_HOLD = 5400

NS_TOKEN = os.environ["PERF_CONTROL_TOKEN"]
SIGNALING = "http://127.0.0.1:19090"
HOST = "198.19.0.10"
CLIENT = "198.19.0.11"
SERVER_CPUS = "0-7"
CLIENT_CPUS = "8-15"
WORKERS = "8"
BUDGET = ["--min-free-memory-mib", "8192", "--min-free-disk-mib", "2048", "--max-server-pss-mib", "16384"]
SAMPLE_INTERVAL = "5"
HERE = pathlib.Path(__file__).resolve().parent
stop_file = OUT / "sampler.stop"

steps = []
owned = []
result = {"status": "RUNNING", "config": {"viewers": VIEWERS, "steady_s": STEADY,
                                          "teardown_s": TEARDOWN, "cycles": CYCLES},
          "steps": steps, "errors": [], "cleanup": {}}


def save():
    (OUT / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2))


def record(name, ok, detail=None):
    steps.append({"step": name, "status": "PASS" if ok else "FAIL", **(detail or {})})
    print(f"[{'PASS' if ok else 'FAIL'}] {name} " + json.dumps(detail, ensure_ascii=False)[:300], flush=True)
    save()


def launch(name, cmd, cpus=None, log=None, env=None):
    argv = ["taskset", "-c", cpus, *cmd] if cpus else list(cmd)
    handle = (OUT / log).open("w") if log else subprocess.DEVNULL
    child_env = dict(os.environ, **(env or {})) if env else None
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
                         env=child_env)
    owned.append((name, p))
    save()
    return p


def wait_listener(port, timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = subprocess.run(["ss", "-lntH", f"sport = :{port}"], capture_output=True, text=True)
        if p.stdout.strip():
            return True
        time.sleep(0.2)
    return False


def proc_identity(pid):
    fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    limits = pathlib.Path(f"/proc/{pid}/limits").read_text()
    # 形如："Max open files            65536                65536                files"（无冒号）
    fd_soft = next((l.split()[3] for l in limits.splitlines()
                    if l.startswith("Max open files")), "?")
    return {"pid": pid, "starttick": int(fields[19]), "exe": os.readlink(f"/proc/{pid}/exe"),
            "netns": os.readlink(f"/proc/{pid}/ns/net"), "fd_soft_limit": fd_soft}


def sleep_mark(label, seconds):
    """分段等待并记录，便于事后按时间轴对齐采样序列。"""
    print(f"[MARK] {label} start {seconds}s mono={time.monotonic():.1f}", flush=True)
    result.setdefault("marks", []).append({"label": label, "at": time.time(),
                                           "mono": time.monotonic(), "seconds": seconds})
    save()
    time.sleep(seconds)
    print(f"[MARK] {label} end mono={time.monotonic():.1f}", flush=True)


def run_play(tag, duration):
    cmd = ["./run.sh", "python3", "bench/perf_run.py", "--output", str(OUT / f"play-{tag}"),
           "--input", "existing", "--sources-file", str(OUT / "publish" / "sources.json"),
           "--signaling-url", SIGNALING, "--server-pid", str(dut_pid), "--control-token", NS_TOKEN,
           "--server-bin", "build/media_server", "--outputs", f"whep:{VIEWERS}",
           "--duration", str(duration), "--warmup", "2", "--progress-interval", "10",
           "--host", HOST, "--client-address", CLIENT,
           "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
           "--workers", WORKERS, "--server-cpus", SERVER_CPUS, "--client-cpus", CLIENT_CPUS, *BUDGET]
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=3600, stdin=subprocess.DEVNULL)
    rf = OUT / f"play-{tag}" / "result.json"
    d = json.loads(rf.read_text()) if rf.is_file() else {}
    g = (d.get("groups") or [{}])[0]
    m = g.get("measurement") or {}
    info = {"tag": tag, "exit_code": p.returncode, "status": d.get("status"),
            "requested": g.get("requested"), "ready": m.get("ready"),
            "progressing": m.get("progressing"), "failed": m.get("failed"),
            "coverage": (g.get("window") or {}).get("coverage"),
            "server_cpu": (d.get("server") or {}).get("cpu_cores"),
            "server_pss_kib": (d.get("server") or {}).get("pss_kib"),
            "window_start_mono": t0, "window_end_mono": time.monotonic(),
            "errors": (g.get("first_errors") or [])[:2]}
    print(f"[PLAY] {tag} {json.dumps(info, ensure_ascii=False)[:400]}", flush=True)
    result.setdefault("plays", []).append(info)
    save()
    return info


try:
    signal.signal(signal.SIGTERM, _terminate)
    signal.signal(signal.SIGINT, _terminate)
    # 1. 真实 signaling（独立数据库）
    launch("signaling", ["./run.sh", "bin/signaling", "-database", str(OUT / "signaling.db"),
                         "-http-listen", "127.0.0.1:19090", "-sip-listen", "127.0.0.1:5060",
                         "-sip-advertise", "127.0.0.1:5060",
                         "-media-control-url", f"http://{HOST}:18080", "-media-control-token", NS_TOKEN,
                         "-media-ip", HOST, "-media-http-port", "18080", "-media-rtmp-port", "11935",
                         "-media-rtsp-port", "18554", "-token-ttl", "60s"],
           CLIENT_CPUS, "signaling.log")
    ok = wait_listener(19090)
    record("signaling_started", ok, {"url": SIGNALING})
    if not ok:
        raise RuntimeError("signaling 未监听")

    # 2. 持久 DUT（本测试拥有，跨 viewer 关闭保持存活）
    # 可选：让 DUT 在退出时打印 jemalloc 统计（区分活跃对象与 allocator retained）。
    # 只影响本测试自己启动的进程环境，不改全局配置。
    jemalloc_env = {"MALLOC_CONF": os.environ["P0M_MALLOC_CONF"]} if os.environ.get("P0M_MALLOC_CONF") else None
    launch("media_server", ["./run.sh", "build/media_server", "--threads", WORKERS,
                            "--bind-address", HOST, "--webrtc-address", HOST,
                            "--rtmp-port", "11935", "--rtsp-port", "18554", "--http-port", "18080",
                            "--control-token", NS_TOKEN, "--signaling-url", SIGNALING],
           SERVER_CPUS, "media_server.log", env=jemalloc_env)
    time.sleep(2.0)
    dut = [p for n, p in owned if n == "media_server"][0]
    dut_pid = dut.pid
    ok = wait_listener(18080) and dut.poll() is None
    identity = proc_identity(dut_pid)
    record("dut_started", ok, {"identity": identity})
    if not ok:
        raise RuntimeError("DUT 未就绪")
    # 每进程 FD 上限会直接限制可服务的观看端数（1024 时约 700 个 WHEP 观看端就耗尽），
    # 因此先断言它已提升，避免把环境限制误读成服务行为。
    if identity["fd_soft_limit"] != "65536":
        raise RuntimeError(f"DUT FD 软限制为 {identity['fd_soft_limit']}，需 65536（由外层 ulimit 提升）")

    # 3. 持续采样（独立进程，逐行 flush，直到 stop 文件出现）
    stop_file = OUT / "sampler.stop"
    samp = launch("sampler", ["python3", str(HERE / "pi_phase6samp.py"), "--pid", str(dut_pid),
                              "--interval", SAMPLE_INTERVAL, "--out", str(OUT / "samples.csv"),
                              "--stop-file", str(stop_file)], None, "sampler.log")
    time.sleep(3)
    record("sampler_started", samp.poll() is None and (OUT / "samples.csv").is_file(),
           {"interval_s": int(SAMPLE_INTERVAL)})

    # 4. 空闲观察
    sleep_mark("idle", IDLE_OBS)

    # 5. 真实发布（1 路 RTMP），保持到测试结束
    pub = launch("perf_publish", ["./run.sh", "python3", "bench/perf_publish.py",
                                  "--output", str(OUT / "publish"),
                                  "--signaling-url", SIGNALING, "--input", "rtmp", "--sources", "1",
                                  "--cycles", "1", "--duration", str(PUB_HOLD), "--ready-timeout", "90",
                                  "--client-address", CLIENT, "--client-cpus", CLIENT_CPUS,
                                  "--client-dir", "build", "--ffmpeg-bin", "bin/ffmpeg",
                                  "--fixture", ".cache/perf_fixture_720p30.flv"],
                 None, "perf_publish.log")
    src = OUT / "publish" / "sources.json"
    deadline = time.time() + 180
    while time.time() < deadline and not src.is_file():
        if pub.poll() is not None:
            break
        time.sleep(1)
    ok = src.is_file()
    record("source_published", ok, {"sources_file": str(src)} if ok else {"publisher_exit": pub.poll()})
    if not ok:
        raise RuntimeError("发布未就绪")
    sleep_mark("source_only", 60)

    # 6. 播放周期
    for c in range(1, CYCLES + 1):
        steady = STEADY if c == 1 else min(300, STEADY)
        info = run_play(f"cycle{c}", steady)
        ok = info["status"] == "PASS" and not info["failed"]
        record(f"play_cycle{c}", ok, info)
        if not ok:
            raise RuntimeError(f"播放周期 {c} 未通过：{info['status']} / {info['errors']}")
        sleep_mark(f"teardown_after_cycle{c}", TEARDOWN)

    # 7. 停止发布者，观察源停止后的水位
    for name, p in owned:
        if name == "perf_publish":
            p.terminate()
    sleep_mark("source_stopped", 60)

    result["status"] = "PASS"
except SystemExit as exc:
    result["status"] = "INTERRUPTED"
    result["errors"].append(f"signal exit {exc.code}")
    print(f"[INTERRUPTED] exit={exc.code}", flush=True)
except Exception as exc:  # noqa: BLE001
    result["status"] = "FAIL"
    result["errors"].append(f"{type(exc).__name__}: {exc}")
    print(f"[FAIL] {type(exc).__name__}: {exc}", flush=True)
finally:
    stop_file.touch()
    for name, p in reversed(owned):
        if p.poll() is None:
            p.terminate()
    deadline = time.time() + 20
    for name, p in reversed(owned):
        if p.poll() is None:
            try:
                p.wait(timeout=max(0.1, deadline - time.time()))
            except subprocess.TimeoutExpired:
                p.kill()
    result["cleanup"] = {"all_owned_processes_reaped": all(p.poll() is not None for _, p in owned)}
    save()

print(json.dumps({"status": result["status"], "cleanup": result["cleanup"]}, ensure_ascii=False))
sys.exit(0 if result["status"] == "PASS" else 1)
