# Pi Agent 性能测试执行手册

## 先读结论

本轮负责工具验收，不交付新容量结果。先读 [verification](verification.md)、[audit](audit.md) 与 [result_format](result_format.md)。CODE VERIFIED、DEV VERIFIED、TEST VERIFIED 是不同证据；TEST 仅完成包部署/校验/依赖/帮助，正式负载、容量及高负载 profiling **NOT RUN / REQUIRES_TEST**。不引用旧最大 viewer 数作为当前成绩。

正式入口：perf_run、perf_publish、perf_validate、perf_capacity、perf_profile；HLS/GB Python 与 RTMP/RTSP/FLV/WHEP/WHIP native 客户端。旧独立 wrapper/GB/browser/lifecycle 授权迁移未完成，见 legacy_tools/functional_verification，不能用其旧 URL 作为正常流程。GB perf_run 是网络 PS 输入，不等于完整 SIP 模拟设备验证。

## A. 环境与归属

DEV 编译打包；TEST `root@172.20.63.32` 执行正式性能。进入 TEST 后先执行 environment.md 的只读检查，记录 uname/lscpu/free、CPU topology、内存/磁盘、工具版本、端口、当前 Pi PID/starttime/namespace。server 0–7、client 8–15、worker 8、地址198.19.0.10/.11 是约定，必须以当次可用环境确认。

不要进入其他任务的 namespace，不停止不属于本轮的进程。只有在确认本轮归属的独立网络空间已准备好后才运行；本工具不自动创建或清理 namespace。客户端本地 UDP 地址与服务端媒体端口地址必须隔离，不能把 EADDRINUSE 当服务器容量瓶颈。不得调全局 sysctl/ulimit/allocator 改善基线。

## B. 构建、打包与校验

使用已经审查的准确 clean 提交，不直接使用活动 Agent 的未提交工作。按 environment.md 构建，记录完整命令、GCC 16.0.1、Boost、FFmpeg SDK/CLI、子模块和 CMakeCache。子模块只初始化锁定版本。

```bash
git rev-parse HEAD
git status --short --untracked-files=all
git submodule status
python3 bench/package_runtime.py --output /absolute/new-runtime-bundle \
  --build-dir build-tooling --fixture /absolute/fixture.flv \
  --whip-fixture /absolute/whip.mkv --ffmpeg-bin /home/gyl/bin/ffmpeg \
  --ffprobe-bin /home/gyl/bin/ffprobe --signaling-bin /absolute/signaling
tar -C /absolute/new-runtime-bundle -czf /absolute/runtime.tar.gz .
sha256sum /absolute/runtime.tar.gz
scp /absolute/runtime.tar.gz root@172.20.63.32:/absolute/new-test-dir/
```

目标目录须由 Pi 自己确认不存在后创建，不覆盖历史结果。TEST 比对传输 SHA 后解包；按 package_runtime.md 的 Python 校验命令检查 files/libraries/NSS 全部 SHA，并保存 manifest 自身 hash。

包内布局：`build/*` 是 native binaries，`bin/ffmpeg`、`bin/ffprobe`、可选 `bin/signaling`；`.cache/perf_fixture_720p30.flv`、可选 `.cache/perf_fixture_whip.mkv`；`bench/*.py`、`lib/*`、`run.sh`、`manifest.json`。包**不含** Chrome、Playwright、perf/BCC/jeprof、宿主 Python/glibc、GB simulator。manifest.head 是打包源码 HEAD；binary_source_head 未记录时不能声称 ELF 的独立编译提交已验证。用实际 ELF SHA 关联构建日志，禁止猜测。

## C. 正式测量前 smoke 与负对照

在确认本轮独立 namespace 内、包根目录执行。下面命令中的安全预算需由 Pi 根据预检自行明确选定；不使用 0 当实际保护。

```bash
./run.sh python3 bench/perf_run.py --help
./run.sh python3 bench/perf_validate.py --help
ROOT=$(mktemp -d /var/tmp/media-perf-pi.XXXXXX)
./run.sh python3 bench/perf_run.py --output "$ROOT/smoke" \
  --input rtmp --outputs flv:1,whep:1 --sources 1 \
  --server-bin build/media_server --client-dir build \
  --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
  --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
  --duration 10 --warmup 2 --progress-interval 5 \
  --min-free-memory-mib 8192 --min-free-disk-mib 2048 --max-server-pss-mib 16384
./run.sh python3 bench/perf_validate.py --output "$ROOT/negative" -- \
  --server-bin build/media_server --client-dir build \
  --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
  --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv
```

默认地址198.19.0.10/.11、CPU0–7/8–15、worker8必须在 namespace 内存在且可用。低负载 ready/逐轨进展、实际完整测量窗口、独立 token、退出码/result 一致、所有 owned process/receiver 清理都要成立。负对照全部 PASS 才能信任拒绝/中断判断，但负对照永不构成容量失败档。

## D. 场景与入口

| 场景 | perf_run 参数（其余用上面相同路径） | 限制 |
|---|---|---|
| publish-only（部分支持） | --input rtmp --outputs none | 整个测量窗口仍有每源1个FLV双轨viewer；只能称publish+validation，不是零输出基线 |
| 单协议 | --input rtmp --outputs rtsp:10 | RTSP播放TCP；每viewer独立token |
| 多源 | --sources 2 --outputs flv:10,whep:2 | 数量必须覆盖所需源，查看实际group/ready |
| 混合 | --outputs rtmp:1,rtsp:1,flv:1,hls:1,whep:1 | HLS最短duration6，独立分片不是逐轨解码 |
| 不同输入 | --input rtsp_tcp / rtsp_udp / rtsp_pull | UDP发布不等于UDP播放或pull |
| WHIP | --input whip --outputs whep:10 | 独立H264/Opus素材，native非浏览器编码 |
| WHEP AAC派生 | --input rtmp --outputs whep:10 | H264/AAC→共享Opus；不关认证 |
| GB接收 | --input gb_udp / gb_tcp_active / gb_tcp_passive --outputs flv:1 | upstream sender→DUTreceiver，不是SIP E2E |
| GB发送 | --input rtmp --outputs gb_udp:1,gb_tcp_active:1,gb_tcp_passive:1 | DUTsender→客户端PS/PES接收，roles见协议文档 |
| 源替换 | perf_publish --cycles 2；perf_run --input existing | 真实Go服务/独立DB；保留每代sources.json，旧播放必须结束，不自动切流 |
| viewer churn | 同一ready源依次运行多个existing短窗 | 每轮新token/新result目录，不重用凭据 |

existing 必须提供真实 `--sources-file --signaling-url --server-pid --control-token`；工具不会关闭外部 DUT。真实发布用 perf_publish.md，Go服务参数核对其当前 help，与media控制Bearer一致。真实服务管理API监听loopback/可信隔离网络，别对外暴露。

## E. 正式容量（本轮未执行）

先 plan-only 校验，不会启动负载：

```bash
./run.sh python3 bench/perf_capacity.py --output "$ROOT/capacity-plan" \
  --protocols whep --start 100 --step 250 --granularity 100 --maximum 5000 \
  --healthy-repeats 3 --failed-repeats 2 --round-timeout 900 --plan-only -- \
  --input rtmp --server-bin build/media_server --client-dir build \
  --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
  --fixture .cache/perf_fixture_720p30.flv --warmup 30 --duration 90 --progress-interval 5 \
  --min-free-memory-mib 8192 --min-free-disk-mib 2048 --max-server-pss-mib 16384
```

检查 result 的 NOT_RUN 计划、资源预算和参数后，**只有 Pi 在正式性能任务中**才能用新的 output 去掉 plan-only。上面maximum只是调度安全上界，不是声称容量5000；server/client先触安全边界即停止。不改变验收判据制造PASS。搜索执行递增→二分→独立H/F复验；`--counts`只固定档位不搜索。H至少3次、F至少2次，所有原始轮次保留。

正式健康：全部请求viewer有真实媒体ready、双轨持续进展、请求窗口至少99%实证覆盖（调度精度界限，见runner）、无确认媒体错误、清理正确。资源预算、客户端CPU/FD/UDPdrop、来源失败、未知失败或矛盾结果都不是服务端F；不确定返回INCONCLUSIVE。CPU高本身不证明容量耗尽。近边界根据真实证据调整后续独立计划，但不修改已经运行的结果或降低媒体标准。

默认预算0只观测，不停止；上述8192/2048/16384MiB是针对约93GiB测试主机的保守执行例，不是已测性能上限，必须经当时预检确认。正式执行前显式加入约定的free-memory/free-disk/server-PSS上限。预算不能代替操作员监控CPU/I/O PSI、客户端及整个host。测试主机压力、媒体停滞、持续队列/丢包、FD/内存逼限、非本轮服务受影响时停止，保留首失败与PID身份，不无界施压。

## F. 独立诊断

不要叠加profilers污染无profiler容量结果。用相同源码、ELF、fixture和目标viewer建立独立诊断轮次，记录实际healthy数。先验证工具/权限，缺失为BLOCKED；不把未采样当零竞争。

```bash
STARTTICK=$(python3 -c 'import pathlib,sys; print(pathlib.Path("/proc/"+sys.argv[1]+"/stat").read_text().rsplit(") ",1)[1].split()[19])' "$DUT_PID")
./run.sh python3 bench/perf_profile.py --mode cpu-stat --output "$ROOT/cpu-stat" \
  --pid "$DUT_PID" --expected-starttick "$STARTTICK" --duration 30
./run.sh python3 bench/perf_profile.py --mode cpu-record --output "$ROOT/cpu-record" \
  --pid "$DUT_PID" --expected-starttick "$STARTTICK" --duration 30
./run.sh python3 bench/perf_profile.py --mode offcpu --output "$ROOT/offcpu" \
  --pid "$DUT_PID" --expected-starttick "$STARTTICK" --duration 30
./run.sh python3 bench/perf_profile.py --mode heap-report --output "$ROOT/heap-report" \
  --binary build/media_server --heap-file /absolute/real.heap
```

heap-report只分析已有真实jemalloc dump，不自动LD_PRELOAD、启用mallocprof或生成阶段快照。先证明allocator参与、profiling能力、dump与ELF匹配，缺能力BLOCKED。perf_record保存raw/report但不自动生成火焰图；Pi按实际可用分析工具独立转换，保留命令/版本/丢样情况。epoll空闲不等于瓶颈，retained不等于泄漏。

## G. 报告与停止

每次交付HEAD/dirty/构建信息、submodules、包/manifest/ELF/fixtureSHA、环境/namespace/CPUaffinity、命令、PID/starttick、actualwindow、ready/progress/failed、资源samples/metrics、日志、全部失败分类/停止原因/cleanup、独立profiling证据和缺失项。不能只提供最大在线数。

容量取消用本轮runnerPID SIGTERM，等待有限清理；驱动保存summary checkpoint，结果INCONCLUSIVE。existing外部DUT/signaling/publisher需由其owner关闭。仅核验本轮PID/starttime后才可强停，不全局pkill/删namespace。结果保留，不把取消写成正式完整窗口。

## H. 已知未完成

- CODE VERIFIED：工具离线、mock故障及状态语义；mock不是媒体或采样实测。
- DEV VERIFIED：小规模真实媒体及运行包功能，见verification实际数值；无容量成绩。
- TEST VERIFIED：独立包传输/SHA/依赖/CLI，不包含性能负载。
- NOT RUN：TEST正式容量、跨机吞吐、高负载On/Off-CPU与jemalloc。
- BLOCKED：需Playwright/Chrome的历史工具未内置，部分旧授权入口不可直接执行；用准确当前入口，不猜参数。

任何新增真实生产缺陷先保留RED，停止依赖该结果的升级；本任务交付不授权重构生产架构或立即实施性能优化。
