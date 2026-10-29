# 阶段一：TEST 正式预检（PASS）

- 时间：2026-10-12T04:32–04:55Z（TEST 本地 12:32–12:55）
- 主机：`seclead-Super-Server`（root@<TEST_HOST>），内核 6.8.0-84-generic，Ubuntu 22.04.5
- 运行目录（TEST）：`/var/tmp/media_server_perf/20261012T043240Z-76355e4`
- namespace：`msperf-20261012T043240Z-76355e4`（本轮创建，`lo` 上 198.19.0.10/11，无路由）
- 未启动任何媒体负载；未修改 sysctl/governor/全局 ulimit；未进入 `mstooling-codex-20261012-b496`、`media-test`。

## 运行包完整性（全部 PASS）

| 项 | 结果 |
|---|---|
| tar SHA256 | `e8f8e298fce58242d3090d9ddbd17b9409dcce889868ac916aa304295efdf905`（与预期一致；传输后二次校验通过） |
| manifest SHA256 | `a3f4e6a08838e90ec0a2655e98032ac14e77dd44596813a413e5a9276a779c0c`（与预期一致） |
| manifest.head | `76355e435a766ed6d6c58be5dc315d14256b4e65`，`dirty=false`，`worktree_status=[]` |
| manifest 条目 | 111 项（47 files + 61 libraries + 3 NSS）逐项 SHA 复算：缺失 0、不一致 0 |
| 8 个 native ELF + ffmpeg/ffprobe/signaling | 全部存在；`ldd` 无 `not found` |
| fixture | `.cache/perf_fixture_720p30.flv` SHA `3e100535…` 与 manifest 一致；WHIP `b40d0f07…` |
| 驱动/客户端 `--help` | perf_run/perf_capacity/perf_validate/perf_publish/perf_profile、rtmp/rtsp/http_flv/whep/whip native、hls_client、gb_sender_fanout 全部退出 0 |
| Python | 3.10.12 + aiohttp 3.14.5 |
| 与历史包不混用 | 全新目录解包；未复用上一轮 `20261011T151927Z-b4965b8-2737042` 的任何文件 |

### ELF 编译来源（不推断，只记录可核验事实）

- `manifest.build.binary_source_head = NOT_RECORDED`，manifest 明确 head 描述的是打包源码树。
- 可核验事实：包内 32 个 Python 入口与 `git 76355e4` 逐字节一致（31 个 `bench/*.py` + `tests/signaling_stub.py`→`bench/signaling_stub.py`）。
- 可核验事实：`build/media_server` mtime 为 2026-10-12 10:47:41，早于 `7706ae2`(11:04)/`21bba8c`(11:26)/`76355e4`(11:30)；`b0b67da..76355e4` 的改动仅为 `bench/*.py`、`tests/*.py`、`docs/`，`CMakeLists.txt` 只新增两条 `add_test` 注册，未改编译输入或编译参数。
- 结论：该 ELF 的 C++ 输入与 `76355e4` 等价；ELF 自身的编译提交仍未被独立记录，本报告不以 manifest.head 冒充 ELF 编译提交。
- 包内 ELF SHA 与 Codex 独立解包副本 `final-runtime-evidence/SHA256SUMS` 全部一致（8/8）。

## 隔离与资源

- CPU：单 socket、NUMA node0（CPU 0-31）。实测 `cpu0↔16`、`cpu7↔23`、`cpu8↔24`、`cpu15↔31`；`lscpu -e` 确认 CPU0-15 属不同物理核。约定 `SERVER_CPUS=0-7`、`CLIENT_CPUS=8-15`、workers 8。
- 背景干扰（10 s `/proc/stat` 实测 busy%）：server 组最大 0.5%、client 组最大 0.1%、siblings 16-31 最大 0.2%。背景任务（systemd/sshd/irqbalance/systemd-oomd/containerd）不受 affinity 约束，可落在 16-31 → **只声明 server 与 client 互不共享物理核，不声明完全物理隔离**。
- 端口：本轮 namespace 内 11935/12035/18554/18654/18080/18180/22000-22005/19090 全部空闲；宿主 14 个监听（sshd/iwpmd/avahi/cupsd/systemd-resolve）不在本轮端口空间，无冲突；无运行容器。
- 资源：MemAvailable 82 507 MiB、磁盘可用 65 255 MiB、PSI cpu/memory/io ≈0。
- 安全预算（本轮固定，同组不变）：`--min-free-memory-mib 8192 --min-free-disk-mib 2048 --max-server-pss-mib 16384`。
- FD：宿主 shell soft nofile=1024（hard 1048576）。本轮只在自己启动的进程树内 `ulimit -Sn 1048576`，不改全局。

## 素材实测（bundled ffprobe）

- 标准 fixture：H.264 **Constrained Baseline** 1280x720 **30 fps** yuv420p ~1.5 Mbps + AAC-LC **48 kHz stereo** ~96 kbps，FLV 12.021 s，**关键帧实测位于第 60/120/180/240/300 帧 → GOP=60**。
- WHIP fixture：H.264 CB 720p30 + **Opus** 48 kHz stereo，MKV 12.039 s（单独分组，不与 AAC 路径同成本）。

## 诊断能力（阶段六准入，实测）

| 能力 | 结果 |
|---|---|
| `perf_profile --mode cpu-stat` | **PASS**（对忙进程，六项计数有效） |
| `perf_profile --mode cpu-record` | **PASS**（perf.data + report，样本存在） |
| `perf_profile --mode offcpu` | **BLOCKED**，两条独立环境缺陷：① `/usr/local/lib/python3.10/dist-packages/bcc/bcc.py` 是 PyPI 的 `bcc` 包，遮蔽系统 BCC 包（import numba/pytest 失败）；② 用 `PYTHONPATH=/usr/lib/python3/dist-packages` 绕过后，BCC 报 `Failed to attach BPF program oncpu to kprobe finish_task_switch`，内核 6.8 实际符号为 `finish_task_switch.isra.0`。未修改全局内核配置。 |
| `heap-report` | 需 `jeprof`（TEST 缺失）+ 真实 heap dump，阶段六另行准备 |

## 阶段一验收

`preflight.json`、`runtime_manifest_verification.json`、`environment.txt`、`cpu_topology.txt`、`network_namespace.txt`、`resource_limits.txt` 及 `tooling_capability.txt`、`other_services.txt`、`fixture_probe.txt`、两份 ffprobe JSON 已归档于本目录 `preflight/`。

**无 P0 阻断，允许进入阶段二。**
