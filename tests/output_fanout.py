#!/usr/bin/env python3
"""Local output fanout / Chrome validation; each run owns its processes and sockets."""
import argparse
import json
import multiprocessing
import os
from pathlib import Path
import selectors
import signal
import socket
import statistics
import subprocess
import time
import urllib.request
import urllib.error
import uuid


def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers={'Content-Type': 'application/json'}), timeout=10) as response:
        raw = response.read()
        return json.loads(raw) if raw else None


def receive(pipe, count, transport):
    selector = selectors.DefaultSelector()
    sockets = []
    ports = []
    counts = [[0, 0, 0, 0, False, None, 0] for _ in range(count)]
    buffers = [bytearray() for _ in range(count)]
    for index in range(count):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if transport == 'udp' else socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.bind(('127.0.0.1', 0))
        if transport != 'udp':
            sock.listen(1)
        sock.setblocking(False)
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
        selector.register(sock, selectors.EVENT_READ, (index, transport != 'udp'))
    pipe.send(ports)
    deadline = None
    started = False
    while deadline is None or time.monotonic() < deadline:
        if pipe.poll():
            deadline = pipe.recv()
            counts = [[0, 0, 0, 0, False, None, 0] for _ in range(count)]
            started = True
        for key, _ in selector.select(0.02):
            index, listening = key.data
            sock = key.fileobj
            if listening:
                connection, _ = sock.accept()
                connection.setblocking(False)
                sockets.append(connection)
                selector.unregister(sock)
                sock.close()
                selector.register(connection, selectors.EVENT_READ, (index, False))
                continue
            packets = []
            try:
                data = sock.recv(65536)
            except BlockingIOError:
                continue
            if not data:
                selector.unregister(sock)
                continue
            if transport == 'udp':
                packets.append(data)
            else:
                buffer = buffers[index]
                buffer.extend(data)
                while len(buffer) >= 2:
                    size = int.from_bytes(buffer[:2], 'big')
                    if len(buffer) < size + 2:
                        break
                    packets.append(bytes(buffer[2:2 + size]))
                    del buffer[:2 + size]
            for packet in packets:
                if len(packet) < 12:
                    continue
                current = counts[index]
                payload = packet[12:]
                if payload.startswith(b'\0\0\1\xba'):
                    for offset in range(14, min(len(payload) - 3, 256)):
                        if payload[offset:offset + 3] == b'\0\0\1' and payload[offset + 3] in (0xe0, 0xc0, 0xbd):
                            current[4] = payload[offset + 3] == 0xe0
                            break
                if not started:
                    continue
                sequence = int.from_bytes(packet[2:4], 'big')
                if current[5] is not None and sequence != current[5]:
                    current[6] += (sequence - current[5]) % 65536
                current[5] = (sequence + 1) % 65536
                current[0] += len(packet) + (2 if transport != 'udp' else 0)
                current[1] += 1
                current[2 if current[4] else 3] += 1
    pipe.send(counts)
    selector.close()
    for sock in sockets:
        sock.close()


def sample(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().split()
    smaps = Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()
    pss = int(next(line.split()[1] for line in smaps if line.startswith('Pss:')))
    return {'cpu': (int(fields[13]) + int(fields[14])) / os.sysconf('SC_CLK_TCK'),
            'pss_kib': pss, 'fd': len(list(Path(f'/proc/{pid}/fd').iterdir())), 'time': time.monotonic()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server')
    parser.add_argument('work_dir')
    parser.add_argument('--mode', choices=['udp', 'tcp', 'chrome', 'whep'], default='udp')
    parser.add_argument('--viewers', type=int, default=100)
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--signaling', default='.cache/output-before-707ca60/signaling')
    args = parser.parse_args()
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=False)
    processes = []
    receivers = []
    logs = []
    result = {}
    source = None
    base = 28400
    signaling_url = f'http://127.0.0.1:{base}'
    control_url = f'http://127.0.0.1:{base + 3}'

    def start(command, name, **kwargs):
        log = (work / (name + '.log')).open('w')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **kwargs)
        processes.append(process)
        return process

    def wait_http(url, process):
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError('process stopped before HTTP ready')
            try:
                urllib.request.urlopen(url, timeout=1).close()
                return
            except urllib.error.HTTPError:
                return
            except Exception:
                time.sleep(0.05)
        raise RuntimeError('HTTP timeout')

    try:
        signaling = start([str(Path(args.signaling).resolve()), '--database', str(work / 'signaling.db'),
                           '--http-listen', f'127.0.0.1:{base}', '--sip-listen', f'127.0.0.1:{base + 4}',
                           '--sip-advertise', f'127.0.0.1:{base + 4}'], 'signaling')
        wait_http(signaling_url, signaling)
        server = start([str(Path(args.server).resolve()), '--threads', '6', '--bind-address', '127.0.0.1',
                        '--webrtc-address', '127.0.0.1', '--media-ip', '127.0.0.1', '--server-id', 'output-validation',
                        '--rtmp-port', str(base + 1), '--rtsp-port', str(base + 2), '--http-port', str(base + 3),
                        '--signaling-url', signaling_url, '--control-url', control_url], 'server')
        wait_http(control_url, server)
        allocation = request(signaling_url + '/api/publish/allocations', {'protocol': 'rtmp', 'stream_name': 'validation/source'})
        publisher = start(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-re',
                           '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=30',
                           '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=44100',
                           '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'libx264', '-preset', 'ultrafast',
                           '-tune', 'zerolatency', '-pix_fmt', 'yuv420p', '-g', '30', '-keyint_min', '30',
                           '-sc_threshold', '0', '-b:v', '2000k', '-maxrate', '2000k', '-bufsize', '4000k',
                           '-c:a', 'aac', '-b:a', '96k', '-ac', '2', '-f', 'flv', allocation['publish_url']], 'publisher')
        time.sleep(3)
        if publisher.poll() is not None:
            raise RuntimeError('publisher failed')
        if args.mode in ('chrome', 'whep'):
            play = request(signaling_url + '/api/play/allocations', {'protocol': 'rtsp', 'stream_name': 'validation/source'})
            source = request(signaling_url + '/api/sources', {'stream_name': 'validation/pull', 'url': play['play_url']})
            request(signaling_url + '/api/sources/' + source['source_id'] + '/start', {})
            time.sleep(3)
            if args.mode == 'chrome':
                completed = subprocess.run(['node', 'tests/chrome_whep_smoke.cjs', signaling_url, source['source_id']], capture_output=True, text=True, timeout=60)
                (work / 'chrome.log').write_text(completed.stdout + completed.stderr)
                if completed.returncode:
                    raise RuntimeError(completed.stdout + completed.stderr)
                result = json.loads(completed.stdout)
            else:
                player = start([str(Path(args.server).resolve().parent / 'fanout_whep_player'), '--signaling-url', signaling_url,
                                '--source-id', source['source_id'], '--viewers', str(args.viewers), '--duration', str(args.seconds),
                                '--ramp-per-second', '100', '--io-threads', '8'], 'whep')
                samples = []
                while player.poll() is None:
                    samples.append(sample(server.pid))
                    time.sleep(1)
                if player.returncode:
                    raise RuntimeError('WHEP player failed')
                result = {'mode': 'whep', 'samples': samples}
            request(signaling_url + '/api/sources/' + source['source_id'] + '/stop', {})
            source = None
        else:
            ports = []
            for count in [args.viewers // 4 + (1 if i < args.viewers % 4 else 0) for i in range(4)]:
                if not count:
                    continue
                parent, child = multiprocessing.Pipe()
                process = multiprocessing.Process(target=receive, args=(child, count, args.mode))
                process.start()
                receivers.append((process, parent))
                ports.extend(parent.recv())
            for index, port in enumerate(ports):
                body = {'stream_id': str(uuid.uuid4()), 'stream_name': 'validation/source', 'sender_id': str(index),
                        'transport': 'udp' if args.mode == 'udp' else 'tcp_active', 'remote_address': '127.0.0.1',
                        'payload_type': 96, 'ssrc': index + 1}
                if args.mode == 'udp':
                    body.update(remote_rtp_port=port, rtcp_enabled=False)
                else:
                    body['remote_port'] = port
                request(control_url + '/gb28181/sender/create', body)
            time.sleep(5)
            log_offset = (work / 'server.log').stat().st_size
            first = sample(server.pid)
            deadline = time.monotonic() + args.seconds
            for _, pipe in receivers:
                pipe.send(deadline)
            samples = [first]
            while time.monotonic() < deadline:
                time.sleep(0.5)
                samples.append(sample(server.pid))
            last = samples[-1]
            counts = []
            for process, pipe in receivers:
                if not pipe.poll(10):
                    raise RuntimeError('receiver timeout')
                counts.extend(pipe.recv())
                process.join(5)
            throughput = sorted(row[0] / args.seconds for row in counts)
            server_log = (work / 'server.log').read_text()[log_offset:]
            result = {'mode': args.mode, 'sessions': args.viewers, 'seconds': args.seconds,
                      'cpu_cores': (last['cpu'] - first['cpu']) / (last['time'] - first['time']),
                      'pss_median_kib': statistics.median(s['pss_kib'] for s in samples),
                      'fd': statistics.median(s['fd'] for s in samples),
                      'gbps': sum(row[0] for row in counts) * 8 / args.seconds / 1e9,
                      'pps': sum(row[1] for row in counts) / args.seconds,
                      'queue_full': server_log.count('queue full'),
                      'video_packets_min': min(row[2] for row in counts), 'audio_packets_min': min(row[3] for row in counts),
                      'sequence_gaps': sum(row[6] for row in counts),
                      'viewer_Bps': {name: throughput[index] for name, index in [('min', 0), ('p10', len(counts) // 10),
                                    ('p50', len(counts) // 2), ('p90', len(counts) * 9 // 10), ('max', len(counts) - 1)]}}
            if not result['audio_packets_min'] or not result['video_packets_min']:
                raise RuntimeError('a viewer has no audio/video progression')
            (work / 'receiver_counts.json').write_text(json.dumps(counts))
            (work / 'samples.json').write_text(json.dumps(samples))
    finally:
        if source:
            request(signaling_url + '/api/sources/' + source['source_id'] + '/stop', {})
        for process, _ in receivers:
            if process.is_alive():
                process.terminate()
            process.join(5)
        for process in reversed(processes):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
        for process in reversed(processes):
            if process.poll() is None:
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    result.setdefault('forced_kill', []).append(process.args[0])
        result['owned_processes_running'] = sum(process.poll() is None for process in processes)
        for log in logs:
            log.close()
        (work / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result))


if __name__ == '__main__':
    main()
