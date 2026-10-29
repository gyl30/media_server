#!/usr/bin/env python3
import json
import sys

for path in sys.argv[1:]:
    r = json.load(open(f"{path}/result.json"))
    server = [p["exit_code"] for p in r.get("processes", []) if p["name"] == "server"]
    print(path.rsplit("/", 1)[-1], r["status"], r.get("capacity_boundary"), "server_exit", server, "server_exit_before_cleanup", r.get("server_exit_before_cleanup"),
          "fault", {k: v for k, v in r.get("fault_evidence", {}).items() if k in ("kind", "media_ready_groups")}, "errors", [e["message"][:90] for e in r["errors"]])
    for g in r["groups"]:
        m = g.get("measurement", {})
        print("   ", g["name"], g["requested"], "progressing", m.get("progressing"), "failed", m.get("failed"), "exit", g["exit_code"],
              g.get("failure_category", ""), [line[:110] for line in g.get("first_errors", [])[:2]])
