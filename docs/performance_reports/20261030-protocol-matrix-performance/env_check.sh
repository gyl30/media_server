#!/bin/bash
# usage: env_check.sh <output.json>; exit 0 = environment ready, exit 1 = BLOCKED (not a server failure)
set -u
NS=${NS:-msperf-20261013T011210Z-7788320}
DUT=${DUT:-198.19.0.10}
CLIENT=${CLIENT:-198.19.0.11}
SERVER_CPUS=${SERVER_CPUS:-0-7}
CLIENT_CPUS=${CLIENT_CPUS:-8-15,24-31}
MEDIA_PORTS="49152 65535"
FIXED_PORTS="11935 18554 18080 12035 18654 18180"
MIN_DISK_MIB=${MIN_DISK_MIB:-6144}
MIN_MEM_MIB=${MIN_MEM_MIB:-16384}
out=${1:?output json}
errors=()
fail() { errors+=("$1"); }

ip netns list | awk '{print $1}' | grep -qx "$NS" || { echo "{\"status\":\"BLOCKED\",\"errors\":[\"namespace $NS missing\"]}" > "$out"; exit 1; }
ip netns exec "$NS" sysctl -qw net.ipv4.ip_local_port_range="32768 49151" || fail "set ip_local_port_range failed"
read -r low high < <(ip netns exec "$NS" sysctl -n net.ipv4.ip_local_port_range | tr -s ' \t' '  ')
[ "$low $high" = "32768 49151" ] || fail "ip_local_port_range is '$low $high'"
read -r media_low media_high <<< "$MEDIA_PORTS"
[ "$high" -lt "$media_low" ] || fail "ephemeral range overlaps media port pool"
host_range=$(sysctl -n net.ipv4.ip_local_port_range | tr -s ' \t' '  ')

ip -n "$NS" link show lo | grep -q "UP" || fail "lo down in $NS"
for address in "$DUT" "$CLIENT"; do
  ip -n "$NS" -4 addr show lo | grep -q "inet $address/" || fail "address $address missing on $NS lo"
  ip -n "$NS" route get "$address" > /dev/null 2>&1 || fail "no route to $address"
done
ip netns exec "$NS" ping -c1 -W1 -I "$CLIENT" "$DUT" > /dev/null 2>&1 || fail "ping $CLIENT -> $DUT failed"

listening=$(ip netns exec "$NS" ss -Hlntu | awk '{print $5}' | sed 's/.*://' | sort -u)
for port in $FIXED_PORTS; do
  grep -qx "$port" <<< "$listening" && fail "port $port already listening"
done
leftovers() { echo $(( $(pgrep -c -x "media_server|whip_publisher|whep_fanout|rtmp_fanout|rtsp_fanout|http_flv_fanout|gb_fanout|rtsp_publisher" || true) +
                         $(pgrep -c -f "bench/(perf_run|hls_client)\.py" || true) )); }
for _ in $(seq 30); do [ "$(leftovers)" -eq 0 ] && break; sleep 2; done
leftover=$(leftovers)
[ "$leftover" -eq 0 ] || fail "$leftover leftover benchmark processes"
sockets=$(ip netns exec "$NS" ss -Hantu | grep -cv TIME-WAIT)
[ "$sockets" -lt 64 ] || fail "$sockets sockets still open in $NS"

measure_busy() { python3 - "$SERVER_CPUS" <<'EOF'
import sys, time
def cpus(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += range(int(a), int(b or a) + 1)
    return out
def read():
    rows = {}
    for line in open("/proc/stat"):
        if line.startswith("cpu") and line[3].isdigit():
            name, *values = line.split()
            values = list(map(int, values))
            rows[int(name[3:])] = (sum(values), values[3] + values[4])
    return rows
server = cpus(sys.argv[1])
siblings = sorted({int(c) for cpu in server for c in cpus(open(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read().strip())} - set(server))
first = read(); time.sleep(2); second = read()
def load(group):
    return max((1 - (second[c][1] - first[c][1]) / max(1, second[c][0] - first[c][0])) for c in group)
print(f"{load(server):.3f} {load(siblings):.3f} {','.join(map(str, siblings))}")
EOF
}
busy=$(measure_busy)
read -r server_busy sibling_busy siblings <<< "$busy"
idle() { python3 -c "import sys; sys.exit(0 if float('$server_busy') < 0.05 and float('$sibling_busy') < 0.05 else 1)"; }
for _ in $(seq 20); do idle && break; busy=$(measure_busy); read -r server_busy sibling_busy siblings <<< "$busy"; done
idle || fail "server cpus or SMT siblings busy ($server_busy/$sibling_busy)"
for cpu in $(python3 -c "print(' '.join(str(c) for p in '$SERVER_CPUS,$CLIENT_CPUS'.split(',') for c in range(int(p.split('-')[0]), int(p.split('-')[-1]) + 1)))"); do
  [ "$(cat /sys/devices/system/cpu/cpu$cpu/online 2>/dev/null || echo 1)" = 1 ] || fail "cpu $cpu offline"
done

disk_mib=$(df -m --output=avail /var/tmp | tail -1 | tr -d ' ')
mem_mib=$(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
[ "$disk_mib" -ge "$MIN_DISK_MIB" ] || fail "disk ${disk_mib} MiB < $MIN_DISK_MIB"
[ "$mem_mib" -ge "$MIN_MEM_MIB" ] || fail "memory ${mem_mib} MiB < $MIN_MEM_MIB"
nofile=$(ip netns exec "$NS" bash -c "ulimit -n 1048576 && ulimit -n" 2>/dev/null || echo 0)
[ "$nofile" -ge 1048576 ] || fail "cannot raise nofile to 1048576"
file_nr=$(awk '{print $1"/"$3}' /proc/sys/fs/file-nr)

status=READY
[ ${#errors[@]} -eq 0 ] || status=BLOCKED
python3 - "$out" "$status" "$NS" "$low $high" "$host_range" "$MEDIA_PORTS" "$DUT" "$CLIENT" "$SERVER_CPUS" "$CLIENT_CPUS" "$siblings" \
  "$server_busy" "$sibling_busy" "$disk_mib" "$mem_mib" "$nofile" "$file_nr" "$sockets" "$(uname -r)" "${errors[@]}" <<'EOF'
import json, sys, time
keys = ["status", "namespace", "ip_local_port_range", "host_ip_local_port_range", "media_port_pool", "dut", "client", "server_cpus", "client_cpus",
        "smt_siblings", "server_cpu_busy_max", "sibling_cpu_busy_max", "disk_free_mib", "mem_available_mib", "nofile", "file_nr", "netns_sockets", "kernel"]
values = sys.argv[2:2 + len(keys)]
record = dict(zip(keys, values), errors=sys.argv[2 + len(keys):], time=time.strftime("%FT%T"))
open(sys.argv[1], "w").write(json.dumps(record) + "\n")
print(json.dumps(record))
EOF
[ "$status" = READY ]
