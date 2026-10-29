## 1 基线与 Git 状态

- BASE HEAD：`891aec1`（生产代码基线）。
- FINAL HEAD：`370de80`，`main` 分支，工作区干净，本地领先 `origin/main` 20 个提交，未推送。
- 工具提交（仅 `bench/`、`CMakeLists.txt` 的 `gb_fanout` 目标和 `docs/performance.md`）：
  `1338812` → `60a96c4` → `68a22ae` → `0418e40` → `6ef7c32` → `bb16e5c` → `86052ae` → `c1fd56e` → `14eaecd` → `4930da7` → `1c2ff7d` → `49cf28b` → `f3f5bfb` → `c64e7d8` → `862ee0c` → `c775ca5` → `cad6ddc` → `939c249` → `551a61c` → `370de80`，内容与验证见第 9 节。
- `git diff 891aec1..HEAD` 中生产目录（`media/`、`signaling/`、`main.cpp` 等）无改动；`CMakeLists.txt` 仅新增 `gb_fanout` 测试目标。
- 被测 ELF：`build/media_server` SHA256 `ef9aab2623df5ad7…`，自 `891aec1` 起未重编，各运行包 manifest 中一致。

## 2 测试环境与 8 核绑定

原始记录：`raw/env/test-host-env.txt`。

- 主机：Xeon Silver 4216，1 路 16 核 32 线程，单 NUMA 节点；L2 1 MiB/核，L3 22 MiB；内存 93.9 GiB，主板 4 个 DIMM 槽（P1-DIMMA1/B1/D1/E1），A1、D1、E1 各一条 32 GB DDR4-2400 RDIMM 生效，B1 装有同型号内存条但 `dmidecode` 报 `Size: Unknown`，未生效；CPU 的 6 个内存通道只用了 3 个，理论带宽 57.6 GB/s（`dmidecode -t memory`）；Ubuntu 22.04，内核 6.8.0-84。
- SMT：物理核 N 与逻辑 CPU N+16 为兄弟线程（`thread_siblings_list` 已逐一核对）。
- 绑定：媒体服务 `--threads 8`，affinity 0-7；兄弟线程 16-23 不放任何测试进程，窗口内 `sibling_busy` 0.007～0.03 核。发布端、viewer、runner、签名桩、上游实例与采集工具使用 8-15,24-31。
- 调频：schedutil，turbo 开启，单核最高 3.2 GHz；8 核满载时实测约 2.70 GHz（APERF/MPERF）。未修改任何全局配置。
- 其他电源与内存状态（`raw/env/test-host-power.txt`）：intel_pstate passive；uncore 频率范围 1.2～2.4 GHz（出厂值，未改）；RAPL PL1 100 W、PL2 120 W；EPB 6；power-profiles-daemon `balanced`；THP `madvise`，defrag `madvise`。
- 功耗与温度（`raw/env/envlog.tsv`，10-24 05:15～15:05 每 5 s 采样 uncore 频率、封装/DRAM 功耗、服务端核频率和封装温度，共 6986 个样本，其中 12 个因 RAPL 能量计数回绕为负值，已剔除）：在服务端核 ≥2.6 GHz 的 2690 个样本中，uncore 中位 2.4 GHz，封装功耗中位 68.4 W、最高 89.6 W，DRAM 功耗 7.5～17.4 W，封装温度最高 51 °C。整个测试期间没有达到功耗墙或温度墙。
- 网络：已有 netns `msperf-20261013T011210Z-7788320`，服务端 198.19.0.10、客户端 198.19.0.11，均在该 netns 的 lo 上。
- 主机既有 TCP 配置：`tcp_wmem=409600 6553600 2147483647`、`tcp_rmem=409600 8738000 2147483647`、`netdev_max_backlog=2500000`、`somaxconn=4096`，均为测试前状态，未修改。
