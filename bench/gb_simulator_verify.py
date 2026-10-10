#!/usr/bin/env python3
"""Verify the GB simulator through public APIs; run stages serially (fixed RTP ports)."""

import argparse
import concurrent.futures
import contextlib
import hashlib
import http.client
import json
import os
import re
import resource
import signal
import sqlite3
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

from fanout_support import benchmark_head, proc_snapshot, stop_process, wait_for_listener, wait_for_stream
from gb_signaling_verify import eventually
from lifecycle_verify import request


def identity(index):
    return f"{34020000001320000001 + index:020d}", f"{34020000001320000002 + index:020d}"


def device_path(index):
    return f"/api/devices/{identity(index)[0]}"


def play_path(index):
    return f"{device_path(index)}/channels/{identity(index)[1]}/play"


def stream_id(run, index):
    return channels(run, index)[0]["live"]["live_id"]


def api(run, method, path, body=None, expected=200):
    status, _, data = request(run["base"] + 3, method, path, body)
    assert status == expected, (method, path, status, data.decode(errors="replace"))
    return json.loads(data) if data else None


def channels(run, index=0):
    return api(run, "GET", device_path(index) + "/channels")["channels"]


def play(run, index=0):
    return api(run, "POST", play_path(index), expected=201)


def consume(run, ticket, expected=404, offer="invalid SDP"):
    response = request(run["base"] + 3, "POST", urlsplit(ticket["whep_url"]).path, offer, "application/sdp")
    assert response[0] == expected, (response[0], response[2])
    return response


def summary(process):
    lines = [line for line in process.log_path.read_text().splitlines() if "simulator summary" in line]
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)(?=\s|$)", lines[-1])} if lines else {}


def sockets(pid):
    owned = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = os.readlink(fd)
            if target.startswith("socket:["):
                owned.add(target[8:-1])
        except FileNotFoundError:
            pass
    ports = set()
    for family in ("udp", "udp6"):
        for line in Path(f"/proc/{pid}/net/{family}").read_text().splitlines()[1:]:
            fields = line.split()
            if fields[9] in owned:
                ports.add(int(fields[1].rsplit(":", 1)[1], 16))
    return {"socket_count": len(owned), "udp_ports": sorted(ports)}


def sample(run, label):
    record = {"label": label, "elapsed_seconds": time.monotonic() - run["started"], "processes": {}}
    for name in ("media", "signaling", "simulator"):
        process = run.get(name)
        if process is None or process.poll() is not None:
            continue
        values = proc_snapshot(process.pid)
        values.pop("threads")
        values.update(sockets(process.pid))
        record["processes"][name] = values
    if run.get("simulator") is not None:
        record["simulator"] = summary(run["simulator"])
    memory = {key: int(value.split()[0]) for key, value in
        (line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())}
    record["host"] = {"mem_available_kib": memory["MemAvailable"], "swap_free_kib": memory["SwapFree"],
        "memory_pressure": Path("/proc/pressure/memory").read_text(),
        "swap_io": {key: int(value) for key, value in
            (line.split() for line in Path("/proc/vmstat").read_text().splitlines()) if key in {"pswpin", "pswpout"}}}
    with (run["output"] / "samples.jsonl").open("a") as output:
        output.write(json.dumps(record) + "\n")
    assert memory["MemAvailable"] > 2 * 1024 * 1024, "host pressure: memory headroom below 2 GiB"
    return record


def mark(run, name, **facts):
    record = {"name": name, **facts}
    run["result"]["checks"].append(record)
    (run["output"] / "result.json").write_text(json.dumps(run["result"], indent=2) + "\n")
    print("PASS", name, json.dumps({key: value for key, value in facts.items()
        if not isinstance(value, (dict, list))}), flush=True)


def launch(run, name, command, expected=0):
    command = list(map(str, command))
    path = run["output"] / f"{len(run['processes']):04d}-{name}.log"
    with path.open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
    process.log_path = path
    run["processes"].append((process, expected))
    run["result"]["commands"].append({"name": name, "command": command, "log": str(path), "pid": process.pid})
    return process


def start_media(run):
    base, args = run["base"], run["args"]
    run["media"] = launch(run, "media", [args.media, "--bind-address", "127.0.0.1", "--webrtc-address", "127.0.0.1",
        "--rtmp-port", base, "--rtsp-port", base+1, "--http-port", base+2, "--threads", "6"])
    wait_for_listener("127.0.0.1", base+2)


def start_signaling(run, heartbeat_timeout=90):
    base, args = run["base"], run["args"]
    run["signaling"] = launch(run, "signaling", [args.signaling, "--database", run["output"] / "devices.db",
        "--sip-listen", f"127.0.0.1:{base+4}", "--sip-advertise", f"127.0.0.1:{base+4}",
        "--http-listen", f"127.0.0.1:{base+3}", "--media-control-url", f"http://127.0.0.1:{base+2}",
        "--media-http-port", base+2, "--heartbeat-timeout", f"{heartbeat_timeout}s"])
    wait_for_listener("127.0.0.1", base+3)


def provision(run, count):
    started = time.monotonic()
    existing = {item["device_id"] for item in api(run, "GET", "/api/devices")["devices"]}
    for index in range(count):
        if identity(index)[0] not in existing:
            api(run, "POST", "/api/devices", {"device_id": identity(index)[0], "name": f"Simulator {index}"}, 201)
    return time.monotonic() - started


def start_simulator(run, count=1, rate=200, endpoints=1, expires=12, heartbeat=1, workers=None, loss=0, profile=None, expected=0):
    args, base = run["args"], run["base"]
    command = [args.simulator, "--platform-sip", f"127.0.0.1:{base+4}", "--listen", f"127.0.0.1:{base+100}",
        "--devices", count, "--sip-endpoints", endpoints, "--register-rate", rate, "--register-expires", f"{expires}s",
        "--heartbeat", f"{heartbeat}s", "--duration", "30s" if profile else "3h", "--packet-loss-percent", loss, "--seed", "42"]
    if workers is not None:
        command += ["--control-workers", workers[0], "--media-workers", workers[1]]
    command += ["--media-profile", profile, "--ffmpeg", args.ffmpeg] if profile else ["--media-file", run["fixture"]]
    run["simulator"] = launch(run, "simulator", command, expected)
    run["simulator"].shutdown_timeout = 35 + (count / rate if rate else 0)
    return run["simulator"]


def stop_simulator(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=process.shutdown_timeout)


def online(run, count, timeout=None):
    expected = {identity(index)[0] for index in range(count)}
    eventually(lambda: expected <= {item["device_id"] for item in api(run, "GET", "/api/devices")["devices"] if item["online"]},
        timeout or max(30, count / 30))
    eventually(lambda: summary(run["simulator"]).get("catalog") == count, timeout or max(30, count / 30))
    assert all(len(channels(run, index)) == 1 for index in range(count))


def offline(run, count):
    expected = {identity(index)[0] for index in range(count)}
    eventually(lambda: not (expected & {item["device_id"] for item in api(run, "GET", "/api/devices")["devices"] if item["online"]}), 30)
    eventually(lambda: all(channels(run, index) == [] for index in range(count)), 30)


def receiver_gone(run, ticket, index=0):
    assert request(run["base"]+2, "POST", "/receivers/delete", {"stream_id": ticket["live_id"]})[0] == 404


def udp_released(run):
    eventually(lambda: not sockets(run["media"].pid)["udp_ports"])


def read_media(run, index=0):
    connection = http.client.HTTPConnection("127.0.0.1", run["base"]+2, timeout=10)
    try:
        connection.request("GET", f"/{stream_id(run, index)}.flv")
        response = connection.getresponse()
        assert response.status == 200
        data = response.read(4096)
        assert len(data) == 4096 and data.startswith(b"FLV"), (index, len(data))
        return len(data)
    finally:
        connection.close()


@contextlib.contextmanager
def browser_page(run):
    from playwright.sync_api import sync_playwright

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=run["args"].browser, headless=True,
            args=["--autoplay-policy=no-user-gesture-required"])
        try:
            page = browser.new_page()
            page.goto(f"http://127.0.0.1:{run['base']+3}/api.js")
            page.set_content("<!doctype html><body></body>")
            page.evaluate("window.viewers = []")
            run["result"]["browser_version"] = browser.version
            yield page
        finally:
            browser.close()


def viewer(page, ticket, require_decode=True):
    index = page.evaluate("""async url => {
        const peer = new RTCPeerConnection(), video = document.createElement('video');
        video.autoplay = video.muted = video.playsInline = true; document.body.append(video);
        peer.addTransceiver('video', {direction:'recvonly'});
        peer.ontrack = event => {video.srcObject = new MediaStream([event.track]); void video.play();};
        await peer.setLocalDescription(await peer.createOffer());
        if (peer.iceGatheringState !== 'complete') await new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('ICE gathering timeout')), 10000);
            peer.onicegatheringstatechange = () => {if (peer.iceGatheringState === 'complete') {clearTimeout(timer); resolve();}};
        });
        const response = await fetch(url, {method:'POST',headers:{'Content-Type':'application/sdp'},body:peer.localDescription.sdp});
        if (response.status !== 201) {peer.close(); video.remove(); throw new Error(`WHEP POST ${response.status}`);}
        const resource = response.headers.get('Location');
        await peer.setRemoteDescription({type:'answer',sdp:await response.text()});
        return viewers.push({peer,video,resource})-1;
    }""", ticket["whep_url"])
    if require_decode:
        eventually(lambda: viewer_stats(page, index)["framesDecoded"] > 0)
    return index


def viewer_stats(page, index):
    return page.evaluate("""async index => {
        const peer = viewers[index].peer, stats = [...(await peer.getStats()).values()];
        const video = stats.find(item => item.type === 'inbound-rtp' && item.kind === 'video');
        return {framesDecoded:video?.framesDecoded || 0,bytesReceived:video?.bytesReceived || 0,
            connectionState:peer.connectionState,dtls:stats.some(item => item.dtlsState === 'connected')};
    }""", index)


def close_viewer(run, page, index):
    resource = page.evaluate("""index => {
        const {peer,video,resource} = viewers[index]; peer.close(); video.srcObject=null; video.remove();
        if (peer.connectionState !== 'closed') throw new Error('peer not closed');
        viewers[index]=null; return resource;
    }""", index)
    endpoint = urlsplit(resource)
    assert request(endpoint.port, "DELETE", endpoint.path)[0] in (204, 404)
    assert request(endpoint.port, "GET", endpoint.path)[0] == 404


def correctness(run):
    args = run["args"]
    with browser_page(run) as page:
        timings = []
        for cycle in range(args.cycles):
            started = time.monotonic()
            provision(run, 1)
            first = start_simulator(run, workers=(2, 1))
            online(run, 1)
            live = play(run)
            wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
            if cycle % 10 == 0:
                index = viewer(page, live)
                close_viewer(run, page, index)
            else:
                read_media(run)
            replay = play(run)
            assert replay["live_id"] == live["live_id"] and replay["play_id"] != live["play_id"]
            api(run, "DELETE", f"/api/lives/{live['live_id']}", expected=204)
            consume(run, live)
            consume(run, replay)
            receiver_gone(run, live)
            next_live = play(run)
            assert next_live["live_id"] != live["live_id"]
            stop_simulator(first)
            offline(run, 1)
            consume(run, next_live)
            receiver_gone(run, next_live)
            second = start_simulator(run, workers=(2, 1))
            online(run, 1)
            new_live = play(run)
            assert new_live["live_id"] not in (live["live_id"], next_live["live_id"])
            api(run, "DELETE", device_path(0), expected=204)
            consume(run, new_live)
            receiver_gone(run, new_live)
            api(run, "GET", device_path(0), expected=404)
            with sqlite3.connect(run["output"] / "devices.db") as connection:
                assert connection.execute("SELECT count(*) FROM gb_devices").fetchone()[0] == 0
            stop_simulator(second)
            rejected = start_simulator(run, workers=(2, 1), expected=1)
            assert rejected.wait(timeout=15) == 1 and "REGISTER challenge status 403" in rejected.log_path.read_text()
            udp_released(run)
            timings.append(time.monotonic()-started)
            if (cycle+1) % 10 == 0:
                mark(run, "single lifecycle progress", cycles=cycle+1, last_seconds=timings[-1], resources=sample(run, "single lifecycle"))
        mark(run, "single lifecycle", cycles=args.cycles, viewer_decode_every=10, timings_seconds=timings)
    provision(run, 1)
    simulator = start_simulator(run, workers=(2, 1))
    online(run, 1)
    for count in args.churn:
        durations, generations = [], set()
        for _ in range(count):
            started = time.monotonic()
            live = play(run)
            assert live["live_id"] not in generations
            generations.add(live["live_id"])
            api(run, "DELETE", f"/api/lives/{live['live_id']}", expected=204)
            consume(run, live)
            receiver_gone(run, live)
            durations.append(time.monotonic()-started)
        udp_released(run)
        mark(run, "play/stop churn", cycles=count, timings_seconds=durations, resources=sample(run, "churn"))
    eventually(lambda: summary(simulator).get("invite") == sum(args.churn) and summary(simulator).get("live_active") == 0)
    for count in (8, 16, 32, 64):
        before = summary(simulator).get("invite", 0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
            tickets = list(pool.map(lambda _: play(run), range(count)))
        assert len({item["live_id"] for item in tickets}) == 1 and len({item["play_id"] for item in tickets}) == count
        eventually(lambda: summary(simulator)["invite"] == before+1 and summary(simulator)["ack"] == before+1 and summary(simulator)["live_active"] == 1)
        api(run, "DELETE", f"/api/lives/{tickets[0]['live_id']}", expected=204)
        for ticket in tickets:
            consume(run, ticket)
        eventually(lambda: summary(simulator)["live_active"] == 0)
        mark(run, "concurrent play single upstream", requests=count, summary=summary(simulator))
    stop_simulator(simulator)
    offline(run, 1)
    udp_released(run)


def ticket_races(run):
    simulator = start_simulator(run, workers=(2, 1))
    online(run, 1)
    live = play(run)
    wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
    with browser_page(run) as page:
        offer = page.evaluate("""async () => {
            const peer = new RTCPeerConnection(); peer.addTransceiver('video', {direction:'recvonly'});
            await peer.setLocalDescription(await peer.createOffer());
            if (peer.iceGatheringState !== 'complete') await new Promise(resolve => {
                peer.onicegatheringstatechange=() => {if (peer.iceGatheringState === 'complete') resolve();};
            });
            const sdp=peer.localDescription.sdp; peer.close(); return sdp;
        }""")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            for rounds in dict.fromkeys((min(100, run["args"].ticket_rounds), run["args"].ticket_rounds)):
                started = time.monotonic()
                for _ in range(rounds):
                    ticket = play(run)
                    responses = list(pool.map(lambda _: request(run["base"]+3, "POST", urlsplit(ticket["whep_url"]).path,
                        offer, "application/sdp"), range(2)))
                    assert sorted(response[0] for response in responses) == [201, 404], responses
                    response = next(response for response in responses if response[0] == 201)
                    resource = urlsplit(response[1]["Location"])
                    assert request(resource.port, "DELETE", resource.path)[0] in (204, 404)
                    consume(run, ticket)
                mark(run, "two concurrent consumers per fresh ticket", rounds=rounds, duration_seconds=time.monotonic()-started,
                    resources=sample(run, "ticket race"))
    for count in run["args"].expiry_batches:
        tickets = [play(run) for _ in range(count)]
        before = sample(run, "tickets pending")
        time.sleep(31)
        for ticket in tickets:
            consume(run, ticket)
        consume(run, play(run), 400)
        assert channels(run)[0]["live"]["live_id"] == live["live_id"]
        mark(run, "unconsumed ticket expiry", tickets=count, wait_seconds=31, before=before, after=sample(run, "tickets expired"))
    api(run, "DELETE", f"/api/lives/{live['live_id']}", expected=204)
    stop_simulator(simulator)
    offline(run, 1)
    udp_released(run)


def lifecycle_races(run):
    count = run["args"].race_rounds
    provision(run, count)
    simulator = start_simulator(run, count, endpoints=min(count, 4), workers=(16, 4))
    online(run, count)
    outcomes = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for index in range(count):
            started = pool.submit(request, run["base"]+3, "POST", play_path(index))
            deleted = pool.submit(request, run["base"]+3, "DELETE", device_path(index))
            play_status, _, data = started.result()
            assert deleted.result()[0] == 204
            assert play_status in (201, 404, 409, 502), (play_status, data)
            if play_status == 502:
                assert json.loads(data)["error"] == "live_start_failed"
            outcomes[play_status] = outcomes.get(play_status, 0)+1
            api(run, "GET", device_path(index), expected=404)
            if play_status == 201:
                ticket = json.loads(data)
                consume(run, ticket)
                receiver_gone(run, ticket, index)
            udp_released(run)
            if play_status == 201:
                assert request(run["base"]+2, "GET", f"/{ticket['live_id']}.flv")[0] == 404
    eventually(lambda: summary(simulator)["live_active"] == 0)
    stop_simulator(simulator)
    udp_released(run)
    mark(run, "delete/play race", rounds=count, outcomes=outcomes, resources=sample(run, "delete/play"))
    provision(run, 1)
    simulator = start_simulator(run, workers=(2, 1))
    online(run, 1)
    outcomes = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for _ in range(count):
            old = play(run)
            stopped = pool.submit(request, run["base"]+3, "DELETE", f"/api/lives/{old['live_id']}")
            started = pool.submit(request, run["base"]+3, "POST", play_path(0))
            assert stopped.result()[0] == 204
            status, _, data = started.result()
            assert status in (201, 409, 502), (status, data)
            if status == 502:
                assert json.loads(data)["error"] == "live_start_failed"
            outcomes[status] = outcomes.get(status, 0)+1
            new = json.loads(data) if status == 201 else None
            if new is None or new["live_id"] == old["live_id"]:
                new = play(run)
            assert new["live_id"] != old["live_id"]
            api(run, "DELETE", f"/api/lives/{old['live_id']}", expected=404)
            assert channels(run)[0]["live"]["live_id"] == new["live_id"]
            api(run, "DELETE", f"/api/lives/{new['live_id']}", expected=204)
            consume(run, new)
            receiver_gone(run, old)
            receiver_gone(run, new)
    stop_simulator(simulator)
    offline(run, 1)
    udp_released(run)
    mark(run, "stop/replay generation race", rounds=count, outcomes=outcomes, resources=sample(run, "stop/replay"))


def steady(run, seconds, label, count, live=0, page=None, viewer_index=None, packet_loss=0):
    started = time.monotonic()
    records = []
    previous = None
    while True:
        record = sample(run, label)
        online_count = sum(item["online"] for item in api(run, "GET", "/api/devices")["devices"])
        assert online_count == count, (label, online_count, count)
        counters = record["simulator"]
        assert counters["registered"] == counters["catalog"] == count, (label, counters)
        assert counters["register_fail"] == counters["heartbeat_fail"] == counters["send_errors"] == 0, counters
        assert counters["live_active"] == live, counters
        assert counters["invite"] == counters["ack"] == live, counters
        if live:
            assert counters["phase_drops"] == 0, counters
        else:
            assert counters["rtp_packets"] == counters["rtp_dropped"] == 0, counters
        record["online"] = online_count
        if page is not None:
            record["viewer"] = viewer_stats(page, viewer_index)
            assert record["viewer"]["connectionState"] == "connected" and record["viewer"]["dtls"]
            if previous is not None:
                assert record["viewer"]["framesDecoded"] > previous["viewer"]["framesDecoded"]
        if previous is not None:
            elapsed = record["elapsed_seconds"] - previous["elapsed_seconds"]
            for name, values in record["processes"].items():
                values["cpu_cores"] = (values["cpu"] - previous["processes"][name]["cpu"]) / elapsed
            if live and packet_loss < 100:
                assert counters["rtp_packets"] > previous["simulator"]["rtp_packets"]
            assert counters["heartbeat_ok"] > previous["simulator"]["heartbeat_ok"]
            assert sum(values["cpu_cores"] for values in record["processes"].values()) < os.cpu_count() * 0.85, "host pressure: sustained CPU saturation"
        with (run["output"] / "steady_samples.jsonl").open("a") as output:
            output.write(json.dumps(record) + "\n")
        records.append(record)
        if len(records) % 6 == 0:
            print("SAMPLE", label, round(time.monotonic()-started),
                json.dumps({name:{key:value for key,value in values.items() if key in {'cpu_cores','rss_kib','fd','socket_count','thread_count','udp_drops'}}
                    for name,values in record["processes"].items()}), flush=True)
        elapsed = time.monotonic()-started
        if elapsed >= seconds:
            break
        previous = record
        time.sleep(min(10, seconds-elapsed))
    return {"duration_seconds": time.monotonic()-started, "samples": records}


def registration_rate(run, count, rate):
    started = time.monotonic()
    log_offset = len(run["signaling"].log_path.read_text())
    simulator = start_simulator(run, count, rate=rate, endpoints=4, expires=120, heartbeat=5)
    try:
        online(run, count)
    except AssertionError:
        if rate != 0:
            raise
        assert simulator.wait(timeout=5) == 1
        counters = summary(simulator)
        logs = run["signaling"].log_path.read_text()[log_offset:]
        dropped = logs.count('msg="Catalog queue full"')
        assert counters["registered"] == count and 0 < dropped == count-counters["catalog"], counters
        assert "Catalog responses" in simulator.log_path.read_text() and "context deadline exceeded" in simulator.log_path.read_text()
        assert "Catalog query failed" not in logs
        run["processes"][-1] = (simulator, 1)
        offline(run, count)
        udp_released(run)
        mark(run, "registration burst Catalog capacity boundary", outcome="CAPACITY_BOUNDARY", devices=count,
            register_rate=rate, catalog_queue_full=dropped, final_summary=counters,
            duration_seconds=time.monotonic()-started, cleanup="all devices offline; channels empty; no media sockets")
        return
    elapsed = time.monotonic()-started
    window = steady(run, 10, f"rate-{rate}", count)
    stop_simulator(simulator)
    offline(run, count)
    mark(run, "registration rate", devices=count, register_rate=rate, register_catalog_seconds=elapsed,
        steady=window, final_summary=summary(simulator))


def control(run):
    args = run["args"]
    for count in args.fleet_counts:
        provisioning = provision(run, count)
        started = time.monotonic()
        simulator = start_simulator(run, count, rate=200, endpoints=min(4, count), expires=120, heartbeat=5)
        online(run, count)
        elapsed = time.monotonic()-started
        window = steady(run, 10, f"register-{count}", count)
        stop_simulator(simulator)
        offline(run, count)
        mark(run, "registration scale", devices=count, register_rate=200, sip_endpoints=min(4,count),
            provision_seconds=provisioning, register_catalog_seconds=elapsed, steady=window, final_summary=summary(simulator))
    count = args.rate_devices
    provision(run, count)
    for rate in (50, 100, 200, 500, 1000, 0):
        registration_rate(run, count, rate)
    for endpoints in (1, 4, 16):
        simulator = start_simulator(run, count, endpoints=endpoints, expires=120, heartbeat=5)
        online(run, count)
        window = steady(run, 10, f"endpoints-{endpoints}", count)
        stop_simulator(simulator)
        offline(run, count)
        mark(run, "SIP endpoint shards", devices=count, endpoints=endpoints, steady=window, final_summary=summary(simulator))
    for count in (100, 500, 1000):
        provision(run, count)
        simulator = start_simulator(run, count, endpoints=4, expires=30, heartbeat=5)
        online(run, count)
        window = steady(run, args.control_soak_seconds, f"keepalive-{count}", count)
        assert summary(simulator)["register_refresh"] >= count
        stop_simulator(simulator)
        offline(run, count)
        mark(run, "control-plane keepalive/refresh soak", devices=count, register_expires=30, steady=window,
            final_summary=summary(simulator), resources_after=sample(run, "control soak stopped"))
    for count in (100, 500):
        provision(run, count)
        for round_number in range(args.offline_rounds):
            simulator = start_simulator(run, count, rate=500, endpoints=4, expires=12, heartbeat=1)
            online(run, count)
            if round_number % 2:
                simulator.kill()
                simulator.wait()
                run["processes"][-1] = (simulator, -signal.SIGKILL)
            else:
                stop_simulator(simulator)
            offline(run, count)
            assert summary(simulator)["catalog"] == count
        mark(run, "fleet offline/online churn", devices=count, rounds=args.offline_rounds,
            alternating="Expires:0 and SIGKILL", resources=sample(run, "fleet churn stopped"))


def media(run):
    provision(run, 1)
    simulator = start_simulator(run)
    online(run, 1)
    live = play(run)
    wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
    with browser_page(run) as page:
        for count in (1, 3, 8, 16):
            indices = [viewer(page, play(run)) for _ in range(count)]
            before = [viewer_stats(page, index) for index in indices]
            page.wait_for_timeout(2000)
            after = [viewer_stats(page, index) for index in indices]
            assert all(b["framesDecoded"] > a["framesDecoded"] and b["bytesReceived"] > a["bytesReceived"]
                for a, b in zip(before, after))
            assert summary(simulator)["invite"] == summary(simulator)["ack"] == summary(simulator)["live_active"] == 1
            for index in indices:
                close_viewer(run, page, index)
            mark(run, "real Chrome viewers share upstream", viewers=count, before=before, after=after,
                summary=summary(simulator), resources=sample(run, "multiple viewers"))
    api(run, "DELETE", f"/api/lives/{live['live_id']}", expected=204)
    stop_simulator(simulator)
    offline(run, 1)
    udp_released(run)
    for count in run["args"].live_counts:
        provision(run, count)
        simulator = start_simulator(run, count, endpoints=min(4,count), expires=30, heartbeat=5)
        online(run, count)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            tickets = list(pool.map(lambda index: play(run, index), range(count)))
        assert len({ticket["live_id"] for ticket in tickets}) == count
        eventually(lambda: summary(simulator)["invite"] == summary(simulator)["ack"] == summary(simulator)["live_active"] == count)
        for index in range(count):
            wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, index))
            read_media(run, index)
        window = steady(run, 10, f"active-live-{count}", count, count)
        for index in range(count):
            read_media(run, index)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(lambda ticket: api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204), tickets))
        for index, ticket in enumerate(tickets):
            consume(run, ticket)
            receiver_gone(run, ticket, index)
        udp_released(run)
        stop_simulator(simulator)
        offline(run, count)
        mark(run, "multi-device active live", devices=count, active_live=count, each_stream_bytes_read=4096,
            steady=window, final_summary=summary(simulator), after=sample(run, "multi-live released"))
    for loss in (0, 1, 5, 10, 100):
        simulator = start_simulator(run, loss=loss)
        online(run, 1)
        ticket = play(run)
        eventually(lambda: summary(simulator)["live_active"] == 1)
        with browser_page(run) as page:
            index = None
            if loss < 100:
                wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
                index = viewer(page, ticket, require_decode=False)
            before = viewer_stats(page, index) if index is not None else None
            window = steady(run, 20, f"packet-loss-{loss}", 1, 1, packet_loss=loss)
            stats = viewer_stats(page, index) if index is not None else None
            if loss == 0:
                assert stats["framesDecoded"] > 0
            if before is not None and before["framesDecoded"] > 0:
                assert stats["framesDecoded"] > before["framesDecoded"]
            if index is not None:
                close_viewer(run, page, index)
        if loss:
            assert summary(simulator)["rtp_dropped"] > 0
        if loss == 100:
            assert summary(simulator)["rtp_packets"] == 0
        api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        receiver_gone(run, ticket)
        udp_released(run)
        stop_simulator(simulator)
        offline(run, 1)
        mark(run, "deterministic packet loss lifecycle", percent=loss, seed=42, steady=window,
            viewer_before=before, viewer=stats, final_summary=summary(simulator))
    for profile in ("normal", "high"):
        simulator = start_simulator(run, profile=profile)
        online(run, 1)
        ticket = play(run)
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
        with browser_page(run) as page:
            index = viewer(page, ticket)
            window = steady(run, 10, f"media-profile-{profile}", 1, 1, page, index)
            close_viewer(run, page, index)
        api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        stop_simulator(simulator)
        offline(run, 1)
        udp_released(run)
        mark(run, "existing generated media profile", profile=profile, steady=window, summary=summary(simulator))
    for workers in (None, (1, 1), (32, 32)):
        provision(run, 10)
        simulator = start_simulator(run, 10, endpoints=4, workers=workers)
        online(run, 10)
        tickets = [play(run, index) for index in range(10)]
        eventually(lambda: summary(simulator)["live_active"] == 10)
        for index in range(10):
            wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, index))
            read_media(run, index)
        window = steady(run, 10, f"workers-{workers}", 10, 10)
        for index in range(10):
            read_media(run, index)
        for ticket in tickets:
            api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        stop_simulator(simulator)
        offline(run, 10)
        udp_released(run)
        mark(run, "representative simulator workers", workers=workers or "default 16/16", steady=window)
    simulator = start_simulator(run, 10, endpoints=4)
    online(run, 10)
    tickets = [play(run, offset) for offset in range(10)]
    for offset in range(10):
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, offset))
        read_media(run, offset)
    stop_simulator(simulator)
    offline(run, 10)
    for offset, ticket in enumerate(tickets):
        consume(run, ticket)
        receiver_gone(run, ticket, offset)
    udp_released(run)
    simulator = start_simulator(run, 10, endpoints=4)
    online(run, 10)
    new = [play(run, offset) for offset in range(10)]
    assert all(a["live_id"] != b["live_id"] for a,b in zip(tickets,new))
    for offset, ticket in enumerate(new):
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, offset))
        read_media(run, offset)
        api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
    stop_simulator(simulator)
    offline(run, 10)
    udp_released(run)
    mark(run, "batch Expires:0 cleans active live/tickets and re-registration starts fresh media", devices=10, active_live=10)


def recovery(run):
    count = 10
    provision(run, count)
    simulator = start_simulator(run, count, endpoints=4)
    online(run, count)
    tickets = [play(run, index) for index in range(3)]
    for index in range(3):
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, index))
        read_media(run, index)
    with browser_page(run) as page:
        index = viewer(page, tickets[0])
        simulator.kill()
        simulator.wait()
        run["processes"][-1] = (simulator, -signal.SIGKILL)
        started = time.monotonic()
        offline(run, count)
        udp_released(run)
        for offset, ticket in enumerate(tickets):
            consume(run, ticket)
            receiver_gone(run, ticket, offset)
        close_viewer(run, page, index)
        mark(run, "simulator SIGKILL expiry cleanup", devices=count, elapsed_seconds=time.monotonic()-started,
            resources=sample(run, "simulator crash cleaned"))
        simulator = start_simulator(run, count, endpoints=4)
        online(run, count)
        old = [play(run, offset) for offset in range(3)]
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
        index = viewer(page, old[0])
        process = run["media"]
        process.kill()
        process.wait()
        position = next(position for position, (item, _) in enumerate(run["processes"]) if item is process)
        run["processes"][position] = (process, -signal.SIGKILL)
        start_media(run)
        for offset, ticket in enumerate(old):
            api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
            receiver_gone(run, ticket, offset)
            consume(run, ticket)
        close_viewer(run, page, index)
        new = [play(run, offset) for offset in range(3)]
        assert all(a["live_id"] != b["live_id"] for a,b in zip(old,new))
        for offset in range(3):
            wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, offset))
            read_media(run, offset)
        index = viewer(page, new[0])
        close_viewer(run, page, index)
        mark(run, "media crash manual stop/replay recovery", active_live=3, automatic_recovery=False,
            resources=sample(run, "media restarted"))

        os.kill(simulator.pid, signal.SIGSTOP)
        stop_process(run["signaling"])
        start_signaling(run)
        assert len(api(run, "GET", "/api/devices")["devices"]) == count
        assert not any(item["online"] for item in api(run, "GET", "/api/devices")["devices"])
        for offset, ticket in enumerate(new):
            assert channels(run, offset) == []
            consume(run, ticket)
            api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=404)
            receiver_gone(run, ticket, offset)
        os.kill(simulator.pid, signal.SIGCONT)
        eventually(lambda: all(item["online"] for item in api(run, "GET", "/api/devices")["devices"]), 30)
        eventually(lambda: summary(simulator)["catalog"] == 2*count)
        current = [play(run, offset) for offset in range(3)]
        mark(run, "signaling restart restores only persistent devices", devices=count, catalog=summary(simulator)["catalog"],
            new_live=True, resources=sample(run, "signaling restarted"))

        os.kill(simulator.pid, signal.SIGSTOP)
        stop_process(run["signaling"])
        stop_process(run["media"])
        start_media(run)
        start_signaling(run)
        for ticket in current:
            consume(run, ticket)
        assert not any(item["online"] for item in api(run, "GET", "/api/devices")["devices"])
        os.kill(simulator.pid, signal.SIGCONT)
        eventually(lambda: all(item["online"] for item in api(run, "GET", "/api/devices")["devices"]), 30)
        eventually(lambda: summary(simulator)["catalog"] == 3*count)
        ticket = play(run)
        assert ticket["live_id"] not in {item["live_id"] for item in current}
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
        index = viewer(page, ticket)
        close_viewer(run, page, index)
        api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        mark(run, "combined backend restart", devices=count, catalog=summary(simulator)["catalog"], resources=sample(run, "combined restart"))

        stop_process(run["signaling"])
        time.sleep(14)
        failed = summary(simulator)
        assert failed["register_fail"] > 0 and failed["heartbeat_fail"] > 0, failed
        start_signaling(run)
        eventually(lambda: all(item["online"] for item in api(run, "GET", "/api/devices")["devices"]), 40)
        eventually(lambda: summary(simulator)["catalog"] == 4*count, 40)
        mark(run, "existing REGISTER refresh recovers signaling outage", before_restore=failed, after_restore=summary(simulator))

        stop_process(run["media"])
        with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
            replies = list(pool.map(lambda offset: request(run["base"]+3, "POST", play_path(offset)), range(count)))
        assert all(reply[0] == 502 for reply in replies)
        assert all("live" not in channels(run, offset)[0] for offset in range(count))
        start_media(run)
        tickets = [play(run, offset) for offset in range(count)]
        for offset in range(count):
            wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, offset))
            read_media(run, offset)
        for ticket in tickets:
            api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        udp_released(run)
        mark(run, "multi-device unavailable media create rollback", devices=count, failed_statuses=[reply[0] for reply in replies],
            recovered_live=count, resources=sample(run, "media create recovered"))
    stop_simulator(simulator)
    offline(run, count)


def soak(run):
    provision(run, 1)
    simulator = start_simulator(run, expires=30, heartbeat=5)
    online(run, 1)
    ticket = play(run)
    wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, 0))
    with browser_page(run) as page:
        index = viewer(page, ticket)
        eventually(lambda: summary(simulator)["live_active"] == 1)
        window = steady(run, run["args"].single_soak_seconds, "single-live-soak", 1, 1, page, index)
        close_viewer(run, page, index)
    api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
    stop_simulator(simulator)
    offline(run, 1)
    udp_released(run)
    mark(run, "single live/viewer soak", steady=window, final_summary=summary(simulator), after=sample(run, "single soak released"))
    count = run["args"].multi_soak_live
    provision(run, count)
    simulator = start_simulator(run, count, endpoints=4, expires=30, heartbeat=5)
    online(run, count)
    tickets = [play(run, offset) for offset in range(count)]
    for offset in range(count):
        wait_for_stream("127.0.0.1", run["base"]+2, stream_id(run, offset))
        read_media(run, offset)
    eventually(lambda: summary(simulator)["live_active"] == count)
    window = steady(run, run["args"].multi_soak_seconds, "multi-live-soak", count, count)
    for offset in range(count):
        read_media(run, offset)
    for offset, ticket in enumerate(tickets):
        api(run, "DELETE", f"/api/lives/{ticket['live_id']}", expected=204)
        receiver_gone(run, ticket, offset)
        consume(run, ticket)
    stop_simulator(simulator)
    offline(run, count)
    udp_released(run)
    mark(run, "multi-device live soak", active_live=count, steady=window, final_summary=summary(simulator),
        after=sample(run, "multi soak released"))


def shutdown_rate(run):
    provision(run, 32)
    simulator = start_simulator(run, 32, rate=1, expires=120)
    online(run, 32, timeout=80)
    started = time.monotonic()
    stop_simulator(simulator)
    remaining = [item["device_id"] for item in api(run, "GET", "/api/devices")["devices"] if item["online"]]
    run["result"]["shutdown_evidence"] = {"devices":32,"register_rate":1,"shutdown_seconds":time.monotonic()-started,
        "still_online":remaining,"returncode":simulator.returncode}
    assert not remaining, run["result"]["shutdown_evidence"]
    offline(run, 32)
    mark(run, "rate-limited fleet exits with all Expires:0 acknowledged", **run["result"]["shutdown_evidence"])


@contextlib.contextmanager
def system(args):
    args.output.mkdir(parents=True, exist_ok=False)
    run = {"args": args, "base": args.port_base, "output": args.output, "started": time.monotonic(), "processes": [],
        "result": {"head": benchmark_head(), "status": "FAIL", "checks": [], "commands": [], "command": sys.argv,
            "environment": {"hostname": os.uname().nodename, "kernel": os.uname().release,
                "cpu_count": os.cpu_count(), "os_release": Path('/etc/os-release').read_text(),
                "python": sys.version, "meminfo": Path('/proc/meminfo').read_text(),
                "rlimit_nofile": resource.getrlimit(resource.RLIMIT_NOFILE)},
            "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for name, path in (("media", args.media), ("signaling", args.signaling), ("simulator", args.simulator))},
            "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}}
    run["fixture"] = args.output / "fixture.h264"
    try:
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
            "-t", "5", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "25", "-bf", "0",
            "-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", str(run["fixture"])], check=True)
        start_media(run)
        start_signaling(run)
        yield run
        run["result"]["status"] = "PASS_WITH_CAPACITY_BOUNDARY" if any(
            check.get("outcome") == "CAPACITY_BOUNDARY" for check in run["result"]["checks"]) else "PASS"
    except Exception as error:
        run["result"]["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        for process, _ in reversed(run["processes"]):
            if process.poll() is None:
                os.kill(process.pid, signal.SIGCONT)
                if hasattr(process, "shutdown_timeout"):
                    stop_simulator(process)
                else:
                    stop_process(process)
        returns = {process.log_path.name: {"actual": process.returncode, "expected": expected} for process, expected in run["processes"]}
        run["result"]["process_returncodes"] = returns
        findings = []
        pattern = re.compile(r"level=(ERROR|WARN)|\[(error|warning)\]|\bfailed\b|queue full|no available media port|protocol_error|cleanup_pending|timeout", re.I)
        for process, _ in run["processes"]:
            matches = [line for line in process.log_path.read_text().splitlines() if pattern.search(line)]
            if matches:
                findings.append({"log": str(process.log_path), "matching_lines": len(matches), "examples": matches[:5]})
        run["result"]["log_findings"] = findings
        run["result"]["duration_seconds"] = time.monotonic()-run["started"]
        errors = {name: value for name, value in returns.items() if value["actual"] != value["expected"]}
        if errors:
            run["result"]["status"] = "FAIL"
            run["result"]["exit_errors"] = errors
        (args.output / "result.json").write_text(json.dumps(run["result"], indent=2)+"\n")
        if errors:
            raise AssertionError(errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media", type=Path, default=Path("build/media_server"))
    parser.add_argument("--signaling", type=Path, required=True)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="/home/gyl/bin/ffmpeg")
    parser.add_argument("--browser", default="/usr/bin/google-chrome")
    parser.add_argument("--port-base", type=int, default=43420)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--churn", type=lambda value: list(map(int, value.split(','))), default=[100, 500, 1000])
    parser.add_argument("--ticket-rounds", type=int, default=1000)
    parser.add_argument("--expiry-batches", type=lambda value: list(map(int, value.split(','))), default=[100, 1000])
    parser.add_argument("--race-rounds", type=int, default=100)
    parser.add_argument("--stage", choices=("correctness", "control", "media", "recovery", "soak", "shutdown", "burst"), default="correctness")
    parser.add_argument("--fleet-counts", type=lambda value: list(map(int, value.split(','))), default=[10, 100, 500, 1000, 2000, 5000, 10000])
    parser.add_argument("--rate-devices", type=int, default=1000)
    parser.add_argument("--control-soak-seconds", type=int, default=600)
    parser.add_argument("--offline-rounds", type=int, default=20)
    parser.add_argument("--live-counts", type=lambda value: list(map(int, value.split(','))), default=[1, 10, 25, 50, 100, 200, 500])
    parser.add_argument("--single-soak-seconds", type=int, default=1800)
    parser.add_argument("--multi-soak-seconds", type=int, default=1200)
    parser.add_argument("--multi-soak-live", type=int, default=100)
    args = parser.parse_args()
    with system(args) as run:
        if args.stage == "correctness":
            correctness(run)
            ticket_races(run)
            lifecycle_races(run)
        elif args.stage == "control":
            control(run)
        elif args.stage == "media":
            media(run)
        elif args.stage == "recovery":
            recovery(run)
        elif args.stage == "soak":
            soak(run)
        elif args.stage == "shutdown":
            shutdown_rate(run)
        elif args.stage == "burst":
            provision(run, args.rate_devices)
            registration_rate(run, args.rate_devices, 0)


if __name__ == "__main__":
    main()
