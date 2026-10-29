#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEDIA = "server_busy=服务端核,mcyc_viewer=Mcycles/viewer,ipc=IPC,gbit_s=媒体 Gbit/s,viewer_kbit_min=viewer 最低 kbit/s,rt_min=实时比例最小,lag_p50=lag span p50 ms,pss_mib=PSS MiB"
TABLES = {
    "whep-control": ("probe_ms_p50=探测 p50,probe_ms_p99=探测 p99,probe_ms_p100=探测最长,probe_over_1s=探测≥1 s,churn_ok=churn 成功轮,"
                     "churn_post_p99=churn POST p99,churn_est_max=churn 建立最长,main_post_p99=爬坡 POST p99,main_est_fail=建立失败,main_ramp_s_max=最长组爬坡 s",
                     "whep3250,whep3500,whep3750"),
    "whep-media": (MEDIA, "whep3250,whep3500,whep3750"),
    "gb-control": ("probe_ms_p50=探测 p50,probe_ms_p99=探测 p99,probe_ms_p100=探测最长,probe_over_1s=探测≥1 s,churn_ok=churn 成功轮,"
                   "main_create_p99=爬坡 create p99,churn_create_p99=churn create p99,main_delete_p99=窗口后 delete p99,main_deleted=删除完成",
                   "gbudp7500,gbudp8000"),
    "gb-media": (MEDIA, "gbudp7500,gbudp8000"),
    "mix": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,ipc=IPC,gbit_s=媒体 Gbit/s,rt_min=实时比例最小,"
            "lag_p50=lag span p50 ms,pss_mib=PSS MiB,probe_ms_p99=探测 p99,main_post_p99=WHEP POST p99,main_create_p99=GB create p99",
            "mix5000,mix7000,mix7250,mix7500"),
    "tcp": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,ipc=IPC,gbit_s=媒体 Gbit/s,viewer_kbit_min=viewer 最低 kbit/s,"
            "rt_min=实时比例最小,lag_p50=lag span p50 ms,pss_mib=PSS MiB,probe_ms_p99=探测 p99",
            "rtmp8500,rtmp8750,rtmp9000,rtsp8500"),
    "cost": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,ipc=IPC,gbit_s=媒体 Gbit/s,lag_p50=lag span p50 ms,pss_mib=PSS MiB",
             "gbtcppassive7000,rtsp6000,flv6000"),
    "select": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,lag_p50=lag span p50 ms,probe_ms_p50=探测 p50,"
               "probe_ms_p99=探测 p99,probe_over_1s=探测≥1 s,main_post_p99=WHEP POST p99,churn_post_p99=churn POST p99,main_create_p99=GB create p99,"
               "main_delete_p99=delete p99",
               "gbtcppassive7000,rtsp6000,mix7000,gbudp8000,whep3500"),
    "gbtcp": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,gbit_s=媒体 Gbit/s,rt_min=实时比例最小,lag_p50=lag span p50 ms,"
              "pss_mib=PSS MiB,probe_ms_p99=探测 p99,main_create_p99=爬坡 create p99",
              "gbtcpactive10000,gbtcpactive11000,gbtcpactive12000,gbtcpactive13000,gbtcppassive12000"),
    "capacity": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,gbit_s=媒体 Gbit/s,viewer_kbit_min=viewer 最低 kbit/s,"
                 "rt_min=实时比例最小,lag_p50=lag span p50 ms,pss_mib=PSS MiB,probe_ms_p99=探测 p99",
                 "gbtcpactive12000,gbtcpactive13000,gbtcpactive13750,gbtcppassive12000,rtmp8500,rtsp7750,mix7250,rtmp9000"),
    "sentinel": ("server_busy=服务端核,mcyc_viewer=Mcycles/viewer,minstr_viewer=Minstr/viewer,ipc=IPC,mhz=MHz,gbit_s=媒体 Gbit/s,lag_p50=lag span p50 ms,pss_mib=PSS MiB",
                 "sn"),
}


def table(name, prefixes, labels):
    spec, points = TABLES[name]
    output = subprocess.run([sys.executable, str(ROOT / "tools" / "abtable.py"), str(ROOT / "raw" / "runs"), spec, points, prefixes, labels or ""],
                            check=True, capture_output=True, text=True).stdout
    return output.strip()


def main():
    report = ROOT / "report.md"
    text = report.read_text()
    pattern = re.compile(r"(<!-- T:([\w-]+) ([\w,-]+)(?: ([\w,=-]+))? -->\n)(.*?)(<!-- /T -->)", re.S)
    text = pattern.sub(lambda match: match.group(1) + table(match.group(2), match.group(3), match.group(4)) + "\n" + match.group(6), text)
    report.write_text(text)


if __name__ == "__main__":
    main()
