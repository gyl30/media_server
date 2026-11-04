import gzip, json, glob, statistics as st, collections, sys
prefix = sys.argv[1]
rows = collections.defaultdict(list)
for f in sorted(glob.glob(prefix + "-*/result.json.gz")):
    n = f.split("/")[0]; p = n.split("-"); v = p[-2]; load = "-".join(p[1:-2])
    r = json.load(gzip.open(f))
    if not r.get("perf_stat"):
        rows[(load, v, "bad")].append(n); continue
    ps = r["perf_stat"]; cb = r["cpu_busy"]; pm = r["process_metrics"]["server"]
    rows[(load, v)].append(dict(gc=ps["cycles"] / ps["seconds"] / 1e9, gi=ps["instructions"] / ps["seconds"] / 1e9, busy=cb["server_busy"], sys=cb["server_system"], soft=cb["server_softirq"],
                                pss=pm["pss_kib_max"] / 1024, p99=r["control_probe"]["ms_p99"], status=r["status"]))
order = sorted({k[0] for k in rows})
print("| 负载 | n base/gso2 | Gcyc/s base | Gcyc/s gso2 | 变化 | Ginstr/s base→gso2 | busy 核 | system 核 | softirq 核 | PSS MiB | 控制 p99 ms（中位） |")
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
for l in order:
    b = rows[(l, "base")]; g = rows[(l, "gso2")]
    if not b or not g: continue
    m = lambda xs, k: st.mean(x[k] for x in xs)
    rng = lambda xs, k: "%.2f–%.2f" % (min(x[k] for x in xs), max(x[k] for x in xs))
    print("| %s | %d/%d | %.2f (%s) | %.2f (%s) | %+.1f%% | %.2f→%.2f (%+.1f%%) | %.2f→%.2f | %.2f→%.2f | %.2f→%.2f | %.0f→%.0f | %.1f→%.1f |" % (
        l, len(b), len(g), m(b, "gc"), rng(b, "gc"), m(g, "gc"), rng(g, "gc"), 100 * (m(g, "gc") / m(b, "gc") - 1), m(b, "gi"), m(g, "gi"), 100 * (m(g, "gi") / m(b, "gi") - 1),
        m(b, "busy"), m(g, "busy"), m(b, "sys"), m(g, "sys"), m(b, "soft"), m(g, "soft"), m(b, "pss"), m(g, "pss"),
        st.median(x["p99"] for x in b), st.median(x["p99"] for x in g)))
for k, v in rows.items():
    if len(k) == 3: print("no perf_stat:", k, v)
