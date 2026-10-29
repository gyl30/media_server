#!/usr/bin/env python3
"""One line per GB UDP sender run: result.json, gb_fanout control/peer lines and the 30 s UDP socket samples (gbs.sh)."""
import json
import re
import sys
from pathlib import Path


def last_sample(log):
    samples, snmp, sockstat, proc = {}, {}, {}, {}
    for line in log.read_text().splitlines() if log.exists() else []:
        match = re.match(r"t=(\d+) (\S+) ?(.*)", line)
        if not match:
            continue
        t, kind, rest = int(match.group(1)), match.group(2), match.group(3)
        if kind == "snmp":
            snmp[t] = [int(value) for value in rest.split()[1:]]
        elif kind == "sockstat":
            sockstat[t] = dict(re.findall(r"(\w+) (\d+)", rest))
        elif kind == "proc":
            proc[t] = rest
        else:
            samples[t] = {name: dict(item.split("=") for item in values.split(",")) for name, values in re.findall(r"(\w+):(\S+)", rest)}
    return samples, snmp, sockstat, proc


def control(run):
    values = {}
    for log in run.glob("*.log"):
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith("phase=control "):
                for key, value in re.findall(r"(\w+)=(\d+)", line):
                    if key.endswith(("p99", "max")):
                        values[key] = max(values.get(key, 0), int(value))
            elif line.startswith("phase=peer "):
                for key, value in re.findall(r"(\w+)=(\d+)", line):
                    values[key] = values.get(key, 0) + int(value)
    return values


def summarize(path):
    run = Path(path)
    result = json.loads((run / "result.json").read_text())
    server = result.get("process_metrics", {}).get("server", {})
    groups = result.get("groups", [])
    requested = sum(group["requested"] for group in groups)
    progressing = sum(group["measurement"].get("progressing", 0) for group in groups)
    realtime = min((group["measurement"].get("viewer_realtime_min", 0) for group in groups), default=0)
    lag = max((group["measurement"].get("viewer_lag_span_ms_max", 0) for group in groups), default=0)
    cpu = result.get("cpu_busy", {})
    probe = result.get("control_probe") or {}
    samples, snmp, sockstat, proc = last_sample(run.parent / f"{run.name}.gbs.log")
    end = max(samples) if samples else None
    fields = [run.name, result["status"], str(result.get("capacity_boundary")), f"prog={progressing}/{requested}", f"rt_min={realtime:.3f}",
              f"lag_max={lag}", f"cores={server.get('cpu_cores', 0):.3f}", f"gcyc={cpu.get('server_gcycles_per_second', 0):.2f}",
              f"pss={server.get('pss_kib_max', 0) / 1024:.0f}M", f"fd={server.get('fd_max', 0)}", f"ev={result.get('server_evidence', {})}"]
    if probe:
        fields.append(f"probe_p99={probe.get('ms_p99', 0):.0f} probe_err={probe.get('errors')}")
    fields += [f"{key}={value}" for key, value in sorted(control(run).items())]
    if end is not None:
        rtcp, rtp = samples[end].get("rtcp", {}), samples[end].get("rtp", {})
        fields += [f"t={end}", f"rtcp_sockets={rtcp.get('sockets')}", f"rtcp_r={int(rtcp.get('r', 0)) / 1048576:.1f}M", f"rtcp_rmax={rtcp.get('r_max')}",
                   f"rtcp_nonempty={rtcp.get('nonempty')}", f"rtcp_drops={rtcp.get('drops')}", f"rtp_r={int(rtp.get('r', 0)) / 1048576:.1f}M",
                   f"rtp_drops={rtp.get('drops')}", f"udp_mem_pages={sockstat.get(end, {}).get('mem')}"]
        first = min(snmp) if snmp else None
        if first is not None and end in snmp:
            names = ["InDatagrams", "NoPorts", "InErrors", "OutDatagrams", "RcvbufErrors", "SndbufErrors"]
            fields += [f"d{name}={snmp[end][index] - snmp[first][index]}" for index, name in enumerate(names) if index in (1, 2, 4, 5)]
        if end in proc:
            fields.append(proc[end].replace(" ", ","))
    return " ".join(fields)


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        try:
            print(summarize(argument))
        except (OSError, ValueError, KeyError) as error:
            print(argument, "ERROR", error)
