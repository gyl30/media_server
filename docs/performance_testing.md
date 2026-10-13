# media_server 性能测试执行规范

> 本文是供 Pi Agent、Codex、Claude Code 及其他 Agent 重复执行的**操作规范**，不是某次测试的结果。适用仓库：`gyl30/media_server`。
>
> 规范版本：v1.2（2026-10-11）；制定时参考 `a753d54cec3ca72e86dc7e4833ce2e0d4e163e55` 的 `bench/`。**实际运行时先检查当前 HEAD、脚本参数和设备能力；不得假定历史 HEAD 即当前版本。**
>
> **固定双机流程**：开发机 `172.20.45.187`（`gyl`）负责 Git worktree、构建、打包、实验修改及报告归档；测试机 `172.20.63.32`（`root`）只负责解包运行、独立 network namespace、固定 8 个物理核心、`--threads 8` 和 profiler。摄像机在 `172.20.63.201:554`，录像准备在性能测试窗口外进行。**禁止在开发机运行正式性能测试。**

## 0.0 两台机器的不可变职责（先读，优先于本文件中的旧单机示例）

| 机器 | SSH / 用户 | 可以做什么 | 不可以做什么 |
|---|---|---|---|
| **DEV 开发/构建机** | `gyl@172.20.45.187`，Pi 当前默认执行位置 | 读取主仓库、创建独立 Git worktree、GCC/CMake 编译、`bench/package_runtime.py` 打包、通过 SSH/SCP 部署、实验性源码修改、保存报告到 `docs/performance_reports/` | 不做容量/混合/压力/On-CPU/Off-CPU 采样；不把开发机 i7-13700KF 的 CPU 拓扑当作目标配置；不要求 DEV `sudo`/root |
| **TEST 专用性能机** | `root@172.20.63.32`，通过 SSH 从 DEV 进入 | 解压运行包、安装诊断工具、基于本机拓扑选 8 个物理核心与客户端 CPU、创建 network namespace、运行所有 benchmark、perf/BCC/eBPF/jemalloc、回传结果、清理运行时资源 | 不就地修改用户主仓库、不用 DEV 的 CPU ID、不用宿主机默认网络进行正式测试 |
| **CAM 摄像机** | `172.20.63.201:554`，凭据另行私下配置 | 在 namespace 之外采集真实素材，固定录像文件及 SHA256 | 不直接作为封闭 namespace 中的正式 RTSP 数据源；不在日志报告中泄露凭据 |

**当前使用者与权限**：Pi 的命令工具默认在 DEV 的 `gyl` 会话，属于正常情况。需要 root 时，应通过已获授权的 `ssh root@172.20.63.32` 在 TEST 执行，**不是让 DEV 用户 `sudo`**。先在 DEV 验证：

```bash
ssh -o BatchMode=yes -o ConnectTimeout=8 root@172.20.63.32 \
  'test "$(id -u)" -eq 0 && hostname && uname -r && lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE && free -h'
```

SSH 失败时先核对实际 SSH 入口、认证及授权，标 `BLOCKED_REMOTE_ACCESS`；**不得转到 DEV 本机测试、不得猜测密码/端口或尝试旁路**。本文件中 `ip netns`、`perf`、`bpftrace`、8 核 `taskset`、网络观测、负载脚本、容量搜索等命令一律在 **TEST** 执行；`git worktree`、CMake/GCC、打包、`scp` 则在 **DEV** 执行。本机 DEV 的 Stage 0 盘点只算 DEV 环境记录，不能当作 TEST 的预检。

## 0. Agent 执行纪律（必须先阅读）

1. **先调查、再 smoke、后基准、再容量、最后诊断。** 每阶段输出结果，遇到阻塞先记录事实；不能为了“跑通”掩盖问题、放宽健康标准或悄悄调整线程数、媒体及测试条件。为定位热点可在独立测试 worktree 中修改代码，但必须按第 9.5 节区分诊断版本与正式基准版本。
2. 所有实际测量、网络隔离、profiling 仅在 TEST 上以 root 执行；DEV 编译无需 root。允许在 TEST 按需安装 `iproute2`、`perf`、`bpftrace`/BCC、`jemalloc` profiling 工具等，但必须记录安装命令、版本和环境变更；先检查是否已安装。**不得默默修改全局 sysctl、网卡参数、防火墙、内核、CPU governor、IRQ affinity 或全局 ulimit。** 如有必要，先记录并取得单独授权。
3. 使用唯一的 network namespace。测试服务器和客户端仅使用 namespace 内的地址和 loopback 网络；没有 veth、NAT、默认路由，也不访问实际摄像机或互联网。**所有基准测试网络流量留在 namespace 内。**
4. 正式基准使用真实摄像机录制形成的、已校验 SHA256 的**本地文件**。获取摄像机录像属于独立素材准备步骤，**不得混入性能测量**；如严格禁止主机网络访问，应由操作者预先交付素材。摄像机原始 URL、凭据和访问令牌不得写入 Git、报告、公开日志或命令记录。
5. **固定资源边界**：同一批可比运行必须使用相同 8 个 CPU ID、8 个 worker、同一套构建参数、codec profile、媒体文件、同一网络拓扑和采样窗口；有任何变化则另立基准组。
6. **不把“连接建立”当成“正常播放”**。必须校验媒体持续增长、音视频轨道、失败计数和会话清理。程序返回码正常但播放停滞，仍判 FAIL。
7. **不将测试工具瓶颈归咎于 media_server**。同步记录 publisher、client、server 的 CPU、调度、丢包及所处 CPU 组；负载工具先饱和时标 `BLOCKED_CLIENT`。
8. **禁止无界施压**。容量越界后可继续超载用于诊断，但必须有压力和磁盘上限；发生 OOM 风险、持续 PSI、日志失控、客户端崩溃或系统不可控时立即停止，不为制造更明显故障盲目加压。
9. **禁止把未经实际执行的测试记为 PASS**。统一状态：`PASS`、`FAIL`、`BLOCKED`、`BLOCKED_CLIENT`、`UNSUPPORTED`、`NOT_RUN`、`INCONCLUSIVE`。保留原始失败，不修改历史结果文件。
10. **报告不进 Git**。从 TEST 回传并校验后，最终报告及必要证据归档在 DEV 的**主工作区**的 `docs/performance_reports/<run_id>/`，目录必须 Git ignored；不要只放在即将删除的测试 worktree 内。临时脚本、原始大日志和中间文件在确认归档有效后删除；临时 namespace、进程及可安全清理的测试 worktree 均需收尾。不可清理用户已有文件或历史报告。
11. **源码修改及构建只在 DEV 的独立测试 worktree 中进行，正式负载与诊断只在 TEST 的运行包中执行**，不占用或改写 Codex 正在使用的主工作区。允许按证据需要修改 `bench/`、测试代码及媒体业务代码来验证性能假设；修改前冻结基线，修改后必须进行同条件 A/B，对每一项改动记录 diff、构建哈希和结果。默认不碰 `third/**`、不自动 commit / push / merge / cherry-pick；实验性修改不等于已批准的生产优化。

## 1. 固定目录与交付

**DEV 主工作区**：`/home/gyl/example/media_server`，仅用于确定基线、读取规范与存放被忽略的最终报告。**DEV 测试 worktree** 从确认的 `BASE_HEAD` 单独创建；编译、测试工具改造和可选源码实验仅在 DEV 的 worktree 完成。**TEST 不需要源码仓库或构建环境，只使用打包后的运行时**；正式性能测试、性能分析和 network namespace 都在 TEST 上执行。

| 类型 | 位置 | 保留策略 |
|---|---|---|
| 长期规范 | 主仓库 `docs/performance_testing.md` | 允许跟踪并随代码版本维护 |
| DEV 隔离测试 worktree | `/var/tmp/media_server_perf/<run_id>/worktree/` | 固定 BASE_HEAD；仅在 DEV 编译和实验，归档后移除 |
| TEST 部署/测量目录 | `/var/tmp/media_server_perf/<run_id>/runtime/` 和 `/var/tmp/media_server_perf/<run_id>/results/` | root 所有；仅运行包、profiling 与原始数据，回传校验后按范围清理 |
| 每轮报告 | **主工作区** `docs/performance_reports/<run_id>/` | **不提交 Git**；即使测试 worktree 清理，报告仍保留 |
| DEV / TEST 临时执行文件 | 两机各自的 `/var/tmp/media_server_perf/<run_id>/` | 仅删除本轮已知文件，且须先完成回传和完整性校验 |
| 原始固定素材 | `/var/lib/media_server_perf/fixtures/` | 首次准备后保留，始终独立于 Git |
| 安装工具 | 系统标准位置 | 记录版本；**不卸载测试前已存在的工具** |

建议在**跟踪的**项目 `.gitignore` 追加（规范落地时执行一次）：

```gitignore
/docs/performance_reports/
```

在上述修改尚未进入当前工作树前，**立即**把同一行添加到本地 `.git/info/exclude`（该文件不被提交），并执行 `git check-ignore -v docs/performance_reports/probe.json` 确认被忽略。禁止用 `git add -f` 强制纳入报告。归档前执行 `git status --short --untracked-files=all`，确认没有报告泄漏到暂存区。

建议归档目录：

```text
docs/performance_reports/<run_id>/
├── README.md                   # 完整测试结论，含 FAIL / BLOCKED
├── manifest.json               # HEAD、二进制、fixture、CPU 集合、工具版本
├── environment.txt             # 脱敏系统与硬件信息
├── matrix.csv                  # 全部计划 case，含未执行项与原因
├── summary.csv                 # 各档位聚合性能及状态
├── runs/
│   └── <case_id>/<repeat>/
│       ├── command.json        # 脱敏后的真实命令和运行参数
│       ├── result.json         # 原始结果（如脚本提供）
│       ├── metrics.csv         # 时间序列或摘要，注明采样间隔
│       └── errors.log          # 限量、脱敏后的错误片段
└── diagnostics/
    └── <case_id>/
        ├── oncpu-perf.data     # 如体积允许，否则保留可复现摘要与路径
        ├── oncpu-report.txt
        ├── offcpu-report.txt
        ├── heap-report.txt
        └── diagnosis.md
```

所有原始大文件先落在 `/var/tmp/...`，测试结束后将脱敏摘要、必要原始证据及实验代码 diff 复制到**主工作区**的报告目录。对于体积过大的原始文件，至少保留统计、时间窗口、可复现命令、截取依据和 SHA256，并说明未保留原始文件。**不要在完成归档和校验前删除临时证据或测试 worktree。**

### 1.1 DEV 独立测试 worktree 的创建与归属

**以下命令在 DEV 的 `gyl` 用户下执行，不需要 root。** 在正式预检确认主工作区后，保存 `MAIN_REPO` 和 `BASE_HEAD`。`MAIN_REPO` 可以存在 Codex 的其他未提交修改，**不允许因此在主工作区执行 reset、restore、clean 或 checkout**。固定测试版本使用 HEAD 中的已提交文件；主工作区未提交的生产代码不自动进入基准。若必须测试它们，先以显式 patch 引入测试 worktree，并标为实验版本。

```bash
MAIN_REPO="$(git rev-parse --show-toplevel)"
BASE_HEAD="$(git -C "$MAIN_REPO" rev-parse HEAD)"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$(git -C "$MAIN_REPO" rev-parse --short HEAD)-$$"
RUN_TMP="/var/tmp/media_server_perf/$RUN_ID"
TEST_WORKTREE="$RUN_TMP/worktree"
REPORT_DIR="$MAIN_REPO/docs/performance_reports/$RUN_ID"
mkdir -p "$RUN_TMP" "$REPORT_DIR"
git -C "$MAIN_REPO" worktree add --detach "$TEST_WORKTREE" "$BASE_HEAD"
cd "$TEST_WORKTREE"
test "$(git rev-parse HEAD)" = "$BASE_HEAD"
git status --short --untracked-files=all
git submodule update --init --recursive
git submodule status
```

- 创建 worktree 之前确认路径不存在、属于本轮唯一 `RUN_ID`；并发 Agent 使用不同 `RUN_ID` 和不同 namespace。
- 读入规范时可直接使用主工作区的本文件；如该文档尚未提交，测试 worktree 内可能没有最新版文件，不代表能擅自合并主工作区其他未提交代码。
- **报告保存到 `$REPORT_DIR`，不要将报告只写进 `$TEST_WORKTREE/docs/`**。主工作区和测试 worktree 都要检查 Git 忽略情况；主工作区 `.git/info/exclude` 或跟踪的 `.gitignore` 必须覆盖报告目录。
- 基线二进制、诊断二进制、实验二进制使用不同构建目录、不同 SHA256，并在报告里分别标识。测试工作区仍可有多个 build 目录，但均位于 DEV 本轮 `$RUN_TMP`，不在主工作区构建。

### 1.2 DEV 构建环境、运行包、跨机部署（强制执行，不得跳过）

仓库已有 [`bench/package_runtime.py`](../bench/package_runtime.py)，会打包：`media_server`、`rtmp_fanout`、`rtsp_fanout`、`whep_fanout`、`rtsp_publisher`、`whip_publisher`、`gb_ps_fanout`、`bench/*.py`、FFmpeg、**一份** fixture 及根据 `ldd` 找到的非系统动态库，并生成 `manifest.json` 和 `run.sh`。`run.sh` 设置 `LD_LIBRARY_PATH` 和 `MEDIA_SERVER_BENCH_HEAD`。**不要只 SCP 一个 `media_server` 可执行文件**：它并非全静态构建，客户端和依赖也必须有对应版本。额外的 fixture/工具若未包含，显式增量部署，不能假定打包器已经包含。

**已知 DEV 构建环境（历史记录，运行时需重新核实）**：Ubuntu 22.04、GCC 16.0.1 experimental（`/usr/bin/gcc-16` 和 `/usr/bin/g++-16`）、CMake 3.31.10、Boost 1.92 静态（`/usr/local/lib/cmake/Boost-1.92.0`）、FFmpeg 编译 SDK 位于 `/home/gyl/ffmpeg901`（libavcodec `63.1.101` 等），FFmpeg 测试程序 `/home/gyl/bin/ffmpeg`（7.1.1）；libSRTP 来自 `third/libsrtp` 静态构建；OpenSSL 静态优先；spdlog/fmt/jemalloc/C++ runtime 等仍可能有动态依赖。**默认系统 pkg-config 会解析到旧版 FFmpeg，因此必须设置正确的 `PKG_CONFIG_PATH` 并记录 `pkg-config --modversion libavcodec`；不从 TEST 编译，不使用 DEV 正被其他进程使用的 `build/`。**

DEV 构建（先完成第 1.1 节，准备真实录像派生出的兼容 fixture，命令中的路径必须存在）：

```bash
# DEV，普通用户 gyl；在 $TEST_WORKTREE 中执行。
cd "$TEST_WORKTREE"
export PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig
BUILD="$RUN_TMP/build_perf"
FFMPEG=/home/gyl/bin/ffmpeg
: "${FIXTURE:?先准备本地固定摄像机录像 fixture，且不可用非摄像机素材冒充正式基准}"
pkg-config --modversion libavcodec libavutil libswresample
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-16 -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
  -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 \
  -DBoost_USE_STATIC_LIBS=ON -DOPENSSL_USE_STATIC_LIBS=TRUE \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON -DBUILD_TESTING=ON \
  -DMEDIA_SERVER_TEST_FFMPEG="$FFMPEG"
cmake --build "$BUILD" -j8
ctest --test-dir "$BUILD" --output-on-failure
sha256sum "$BUILD"/{media_server,rtmp_fanout,rtsp_fanout,whep_fanout,whip_publisher,rtsp_publisher}
python3 bench/package_runtime.py \
  --build-dir "$BUILD" --fixture "$FIXTURE" \
  --ffmpeg-bin "$FFMPEG" --output "$RUN_TMP/runtime"
tar -C "$RUN_TMP" -czf "$RUN_TMP/runtime.tar.gz" runtime
(cd "$RUN_TMP" && sha256sum runtime.tar.gz > runtime.tar.gz.sha256)
```

素材建议：在 **TEST 的宿主网络**采集摄像机录像（不进入 namespace），生成标准 H.264/AAC 版本，然后将该版本 SCP 到 DEV 的 `$RUN_TMP/fixtures/` 用于打包；或者在 DEV 的宿主网络完成录制。**不要把摄像机账号密码放在远程命令、环境清单或报告中**，采集完成后仅转移视频文件与 FFprobe/SHA256 信息。TEST 的 `/var/lib/media_server_perf/fixtures/` 可以长期保留原始素材；正式测试运行包的固定文件位于 `runtime/.cache/perf_fixture_720p30.flv`，并核对内容与 SHA256，绝不能只看文件扩展名。

DEV 将运行包部署到 **TEST**（以下 SSH 示例要求授权的 root 密钥登录；如果不可达标 `BLOCKED_REMOTE_ACCESS`，不在 DEV 上运行测试）：

```bash
TEST_SSH=root@172.20.63.32
ssh -o BatchMode=yes "$TEST_SSH" "install -d -m 700 /var/tmp/media_server_perf/$RUN_ID"
scp "$RUN_TMP/runtime.tar.gz" "$RUN_TMP/runtime.tar.gz.sha256" \
    "$TEST_SSH:/var/tmp/media_server_perf/$RUN_ID/"
ssh "$TEST_SSH" "set -eu; cd /var/tmp/media_server_perf/$RUN_ID; \
  sha256sum -c runtime.tar.gz.sha256; tar -xzf runtime.tar.gz; \
  ./runtime/run.sh ./bin/ffmpeg -version; \
  ./runtime/run.sh python3 -c 'import aiohttp'"
```

**TEST 预检**：进入 `ssh root@172.20.63.32` 后，令 `RUN_TMP=/var/tmp/media_server_perf/<run_id>`、`BUNDLE=$RUN_TMP/runtime`、`BUILD=$BUNDLE/build`、`FFMPEG=$BUNDLE/bin/ffmpeg`、`FIXTURE=$BUNDLE/.cache/perf_fixture_720p30.flv`。`./runtime/run.sh` 会切换到 bundle 根目录并设置 bundled lib 搜索路径。验证所有受测 ELF 的 `ldd` 无 `not found`，验证 Python `aiohttp`、实际 FFmpeg、fixture SHA256、部署二进制 SHA256 和正常启动/退出。若 TEST 的 glibc/动态库或发行版不兼容，记录具体 loader 错误，修复**私有运行包或编译目标兼容性**；不要升级/覆盖全局 libc，不要绕过检测。严禁把文件拷到 DEV 的运行包目录后就宣称已部署到 TEST。

**TEST 启动原则**：在 TEST 的 SSH root 会话中创建 namespace、CPU pin wrapper，并从 bundle 调用测试，例如：

```bash
# TEST，root；变量从本轮部署目录和 TEST lscpu 实测获取。
RUN_TMP="/var/tmp/media_server_perf/<run_id>"
BUNDLE="$RUN_TMP/runtime"; BUILD="$BUNDLE/build"
FIXTURE="$BUNDLE/.cache/perf_fixture_720p30.flv"
FFMPEG="$BUNDLE/bin/ffmpeg"; SERVER_REAL="$BUILD/media_server"
export RUN_TMP SERVER_REAL SERVER_CPUS CLIENT_CPUS
: "${NS:?先按第 4 节创建 TEST 的 namespace}"
cd "$BUNDLE"
ip netns exec "$NS" taskset -c "$CLIENT_CPUS" \
  "$BUNDLE/run.sh" python3 bench/rtmp_fanout.py \
  --fixture "$FIXTURE" --server-bin "$RUN_TMP/media_server_pinned" \
  --client-bin "$BUILD/rtmp_fanout" --ffmpeg-bin "$FFMPEG" \
  --host 198.19.0.10 --workers 8 --viewers 1 --warmup 5 --duration 15 \
  --output "$RUN_TMP/results/smoke-rtmp"
```

所有子进程必须继承 `run.sh` 的动态库环境。`SERVER_CPUS` 与 `CLIENT_CPUS` 必须来自 **TEST 本机**，不得沿用 DEV 的 `0,2,...,14` 与 `16-23`。若 `performance_baseline.py` 的 `--workers 6` 硬编码、`mixed_fanout.py` 的相对路径或时间窗口不满足规范，**在 DEV worktree 最小修改、重新编译/打包/部署**，保留每版二进制、脚本和包 SHA。不得直接修改 TEST 包里的生产可执行文件而不留源代码证据。

**TEST → DEV 回传及清理**：先停止 TEST 本轮进程并销毁 namespace；将 TEST 的 `$RUN_TMP/results/`、环境文件、必要的 perf/heap 原始证据及实验版 SHA 回传 DEV 的 `$REPORT_DIR`（例如从 DEV 执行 `scp -r root@172.20.63.32:/var/tmp/media_server_perf/<run_id>/results/. "$REPORT_DIR/"`），核对文件数量、大小和 SHA256，再清理 TEST 的**本轮**临时包与构建运行目录。DEV 最后归档 diff、实验文件、A/B 结果，清理 DEV worktree 和临时构建；主仓库、固定 fixture、最终报告保持不变。

## 2. 双机预检（先检查 TEST，第一阶段不运行容量）

**DEV（gyl 用户）**：执行 `git rev-parse HEAD`、`git status --short --untracked-files=all`、`git submodule status`、编译器/Boost/FFmpeg SDK/`pkg-config`/CMake 检查；创建第 1.1 节的 worktree；执行第 1.2 节的构建及打包。不要在 DEV 执行 network namespace、容量/超载或性能 profiling。

**TEST（通过 SSH root）**：保存 `hostname`、`id -u`、`uname -a`、`lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE`、SMT sibling、`free -h`、`df -h`、`perf --version`、`perf list`、`bpftrace --version`、`jeprof`、`ffmpeg -version`、`/proc/pressure/*`、当前监听和进程信息。确认 TEST 无与本轮冲突的运行负载，依照 TEST 拓扑选择 8 个物理核心以及不相交的客户端核心，保存 CPU IDs，不使用 DEV 的 P/E 配置。

只有 TEST 的真实 namespace、CPU 分配、运行包依赖、FFmpeg 和 Python 客户端检查通过，才继续 Stage 1。所有结果明确写明 `machine=DEV` / `machine=TEST`，**不能把两台机器的指标放进同一个测试机环境信息记录**。TEST 的 root 权限不足或 SSH 失败必须标 BLOCKED，不得以 DEV 的无 root 结果代替。

## 3. 固定摄像机媒体文件

1. 优先使用摄像机真实录像，录制时不进行编码，保留时间戳变化特征。录像准备和 `ffprobe` 放在**正式测试窗口之外**。
2. 采集原始文件之后，离线生成标准 H.264 + AAC profile，首轮以 720p30、约 2 Mbps、GOP=60、AAC 48 kHz 双声道为目标。保持固定文件、码率、GOP、音轨和参数，不采用实时编码。
3. 媒体特征记录：`sha256sum`、`ffprobe` JSON、实际编码和 profile、宽高、平均帧率、音频采样率、时长、峰值及平均码率、关键帧分布。
4. 视频-only、H.265、G.711A/U 作为专项 profile；不支持的交叉组合明确 `UNSUPPORTED`。
5. 正式 benchmark 只使用本地文件 + `-stream_loop -1 -re -c copy` 方式发布，且验证服务器实际收到符合要求的音视频流。

参考录制命令（**仅素材准备阶段使用**，摄像机凭据应来自安全私有配置；**注意 FFmpeg 的输入 URL 仍可能出现在进程命令行**）：

```bash
ffmpeg -hide_banner -rtsp_transport tcp -i "$CAMERA_RTSP_URL" \
  -map 0 -c copy -t 900 camera_native.mkv
ffprobe -v error -show_format -show_streams -of json camera_native.mkv \
  > camera_native.ffprobe.json
sha256sum camera_native.mkv
```

如摄像机源不能直接兼容所有输出，录制文件离线转码一次，生成稳定的 `camera_standard.mkv`。离线处理参数与版本归档；不推荐代理自动挑选编码 preset 或重新生成素材而不更新 `fixture_sha256`。

**完全封闭的基准 namespace 无法直接访问 `172.20.x.x` 摄像机。** 正式 RTSP pull 输入性能使用 namespace 内的第二个 RTSP 源服务，播放已保存的摄像机录像，DUT 经 `/rtsp/pull/create` 拉取。实时摄像机直连属于另外的真实网络集成实验，不能与封闭 namespace 性能数值合并。

## 4. 一次性网络 namespace（强制）

采用**一个独立 namespace + 其内部 `lo` 上两个测试 IP**。测试程序及其启动的所有客户端都进入同一 namespace。无需创建 veth、bridge、NAT，也不为 namespace 配置 default route，避免与宿主机现有业务监听端口冲突。

推荐 namespace 地址：

- 服务器所有监听接口：`198.19.0.10`（只配置在 namespace 内，不是宿主机地址）。
- 客户端显式绑定地址：`198.19.0.11`（主要用于 WHEP UDP 测试）。
- IP 属于 benchmark 用地址范围，仅在独立 namespace 内使用；实际只经 namespace `lo` 传输。

创建前确保唯一命名；**不删除同名的未知 namespace**。示例：

```bash
# TEST root；RUN_ID 来源于 DEV，TEST 的 RUN_TMP 为 /var/tmp/media_server_perf/<run_id>，不要自行生成另一 ID。
NS="msperf-${RUN_ID}"
# SERVER_CPUS / CLIENT_CPUS 必须先依据 lscpu 拓扑手动选定；例如形式为 '0,2,4,...'。
: "${SERVER_CPUS:?must_select_8_physical_cores}"
: "${CLIENT_CPUS:?must_select_disjoint_cores}"
SERVER_REAL="$BUILD/media_server"
export RUN_TMP SERVER_CPUS CLIENT_CPUS SERVER_REAL

ip netns add "$NS"
ip -n "$NS" link set lo up
ip -n "$NS" address add 198.19.0.10/32 dev lo
ip -n "$NS" address add 198.19.0.11/32 dev lo
ip netns exec "$NS" ip -br address
ip netns exec "$NS" ip route
ip netns exec "$NS" ss -lntup
ip netns exec "$NS" ping -c 1 -W 1 198.19.0.10
```

**实际监听必须使用 `--bind-address 198.19.0.10 --webrtc-address 198.19.0.10`**（通过现有 bench 脚本的 `--host 198.19.0.10` 传入），不能使用宿主机真实地址。WHEP client 用 `--client-bind-address 198.19.0.11`。没有外网路由不代表能无视客户端自己的绑定参数；所有测试命令都应检查。

脚本自建服务的端口仍需在 namespace 内查冲突；独立 namespace 防止**宿主机**服务占用相同端口，不防止同一个 namespace 内两个测试进程抢同一端口。一次只跑一个正式负载，混合负载则由统一协调器启动。

如果上面 ping（需 `iputils-ping`）、监听、WebRTC UDP 建连失败，先做 namespace 内最小连通性验证，标 `BLOCKED`；不允许为了跑通就切回宿主机网络。

### 4.1 固定 CPU 的最小启动方式

现有 fanout Python 脚本会自己启动服务器、FFmpeg 和客户端。**不能简单地把整个 Python 命令用 `taskset SERVER_CPUS` 启动**，否则客户端也继承服务器 CPU，测量失真。

推荐方式：**仅在 TEST 的 root 会话**中，外层 runner 用 `CLIENT_CPUS`，为 `--server-bin` 提供一个仅负责服务器亲和性的 wrapper。Wrapper 存在于 TEST 临时目录，**不提交 Git**：

```bash
# 示例中的变量必须来自本机 lscpu 实测结果：
# export SERVER_CPUS='...' CLIENT_CPUS='...'
# export SERVER_REAL='/absolute/path/to/build_perf/media_server'
# export RUN_TMP='/var/tmp/media_server_perf/<run_id>'
cat > "$RUN_TMP/media_server_pinned" <<'SH'
#!/bin/sh
set -eu
: "${SERVER_CPUS:?}"
: "${SERVER_REAL:?}"
: "${RUN_TMP:?}"
printf '%s\n' "$$" > "$RUN_TMP/server.pid"
# 仅服务器启用 jemalloc 专项 profiling；普通基准保持 unset。
if [ -n "${BENCH_MALLOC_CONF:-}" ]; then
  export MALLOC_CONF="$BENCH_MALLOC_CONF"
fi
exec taskset -c "$SERVER_CPUS" "$SERVER_REAL" "$@"
SH
chmod 700 "$RUN_TMP/media_server_pinned"
```

在 **TEST** 启动随运行包分发的 `bench/*.py` 时，`SERVER_CPUS`、`SERVER_REAL`、`RUN_TMP` 已按上文导出；`BUILD` 是 TEST 的 `$BUNDLE/build`，`FIXTURE` 是 TEST 的 `$BUNDLE/.cache/perf_fixture_720p30.flv`，`FFMPEG` 是 TEST 的 `$BUNDLE/bin/ffmpeg`。确认它们都非空且可访问，例如：

```bash
ip netns exec "$NS" taskset -c "$CLIENT_CPUS" \
  "$BUNDLE/run.sh" python3 bench/rtmp_fanout.py \
  --fixture "$FIXTURE" --server-bin "$RUN_TMP/media_server_pinned" \
  --client-bin "$BUILD/rtmp_fanout" --ffmpeg-bin "$FFMPEG" \
  --host 198.19.0.10 --workers 8 --viewers 1 \
  --warmup 15 --duration 60 --output "$RUN_TMP/smoke-rtmp-1"
```

`BUILD`、`FFMPEG`、`FIXTURE` 使用绝对路径，先 `readlink -f` 核对存在；不要将示例命令不加检查地复制执行。由于 wrapper 使用 `exec`，`server.pid` 对应真正服务端 PID；采样前再次以 `/proc/<pid>/exe` 和 `ps` 核实。

在测试中检查 `taskset -pc <server_pid>` 和每个 `/proc/<pid>/task/*/status` 的 `Cpus_allowed_list`；同时检查客户端和 publisher affinity。**仅设置亲和性不保证实时线程一定有足够 CPU；负载机已饱和时必须记录。**

### 4.2 namespace 清理（包括失败、超时、中断）

所有启动器必须有 `finally` / shell `trap`。按顺序：停止本轮客户端 → 停止本轮 publisher → 正常终止本轮服务器 → 等待进程退出 → 检查 namespace 内遗留 PID → 删除 namespace → 验证不存在。

```bash
ip netns pids "$NS" || true
# 先用本轮保存的 PID 和进程组优雅退出；超时后再仅处理本轮进程。
# 必须确认 namespace 仅由本轮创建且不含其他工作负载。
ip netns del "$NS"
ip netns list
```

注意：`ip netns del` 删除名称**不代表仍有进程持有的 namespace 已销毁**。如果 `ip netns pids "$NS"` 仍有进程，先处理残留 PID，不允许宣称清理成功。绝不能用全机 `pkill media_server`、`killall ffmpeg`，也不能删除非本轮 namespace。

## 5. 测试矩阵：先兼容，再性能

### 5.1 输入（发布 / 拉取）

`RTMP publish`；`RTSP publish TCP`；`RTSP publish UDP`；`RTSP pull TCP`；`WHIP publish`；`GB28181 receive UDP`；`GB28181 receive TCP active`；`GB28181 receive TCP passive`。

### 5.2 输出（消费 / 推送）

`RTMP`；`RTSP TCP`；`HTTP-FLV`；`HLS`；`WHEP`；`GB28181 sender UDP`；`GB28181 sender TCP active`；`GB28181 sender TCP passive`。

`RTSP UDP playback` 不应自行假设已实现；确认代码及客户端能力后再列入。RTSP push/pull 是不同输入类型。GB28181 TCP active/passive 必须在建连方向上分别覆盖。WHEP 的 Opus 音频与 RTMP/RTSP AAC 不相同，必须记录转码路径。

**兼容性阶段**：对每个输入 × 输出 × codec profile 先只运行 1 source × 1 viewer 的真实协议 smoke，确认音视频持续增长，结果标 `SUPPORTED/PASS`、`UNSUPPORTED`、`FAIL` 或 `BLOCKED`。未验证的组合不能直接进入容量测试；不能把 unsupported 当性能故障。

**性能阶段**：优先使用统一 RTMP 输入，分别测 RTMP/RTSP/HTTP-FLV/HLS/WHEP/GB 输出，确保协议边际成本可比较。其他输入协议在同样媒体参数下另测输入成本和典型组合。RTSP pull 与 GB 必须使用 namespace 内的本地源/接收器，不可访问外部设备。

### 5.3 负载模式

| 代号 | 模式 | 目的 |
|---|---|---|
| B0 | 空闲服务器 | CPU/RSS/PSS/FD 空载基线 |
| B1 | 1 路输入、零个长期 viewer | 输入协议基线 |
| B2 | 1 source × N viewer，同一种输出 | 每观众边际成本、单源 fanout |
| B3 | S sources × V viewers/source | 多源扩容与跨 worker 调度 |
| B4 | 1 source × 多种输出协议 | 共享转码/共享 PS/协议互相影响 |
| B5 | 多 sources × 多协议混合 | 综合业务和热点源分布 |
| B6 | 可控 churn、source replacement、长时 soak | 资源回收、稳态、代际隔离 |
| D | 经过确认的健康/失稳邻近负载 | perf On-CPU/Off-CPU/jemalloc 根因定位 |

优先比较总 viewer 数相同的 `1×100`、`10×10`、`100×1`，以及混合协议中 WHEP/TCP/HLS/GB 主导的负载。HLS 必须记录实际 playlist/segment HTTP 请求与完整响应下载；**不能简单将 HLS viewer 数视为持续推送连接数**。

## 6. 现有 bench 工具的可靠使用范围

| 路径 | 当前能力 / 注意事项 |
|---|---|
| `bench/publish_only.py` | RTMP 输入基准，支持 `--workers`、`--sources` |
| `bench/rtmp_publish.py` | RTMP 多源输入 |
| `bench/rtsp_publish.py` | RTSP TCP/UDP 多源输入；使用合成 publisher，输入条件需先校准 |
| `bench/whip_publish.py` | WHIP 输入；合成 H.264/Opus publisher，不与真实文件无条件等价 |
| `bench/rtmp_fanout.py` | RTMP 输入 → RTMP 输出 |
| `bench/rtsp_fanout.py` | RTMP 输入 → RTSP TCP 输出（native client 当前使用 TCP） |
| `bench/http_flv_fanout.py` | RTMP 输入 → HTTP-FLV 输出 |
| `bench/hls_fanout.py` | RTMP 输入 → HLS；另有 `--persistent` 模式 |
| `bench/whep_fanout.py` | RTMP 输入 → WHEP；同机 namespace 需显式 `--client-bind-address` |
| `bench/gb_network.py` | GB UDP/TCP 网络 pair 专项；不能直接等同“GB 输出健康观众最大容量” |
| `bench/gb_sender_fanout.py` | 现有已发布媒体流 → GB UDP/TCP active sender；需要控制端点及独立 source |
| `bench/lifecycle_verify.py` | smoke、replacement、churn、soak、disconnect 的功能压力 |
| `bench/codec_interop_verify.py` | codec 组合互通验证；**不是全协议容量脚本** |
| `bench/performance_baseline.py` | **当前内置 `--workers 6`，原样不能作为本规范的 8-worker 主基准** |
| `bench/mixed_fanout.py` | **当前只覆盖 RTMP/RTSP/WHEP/HLS**，客户端 `build/*` 路径硬编码，server/client 测量窗口不同，健康检查不完整；不能直接用于正式混合性能排名 |

Agent 按现有实际 `--help` 和源码选择工具，优先使用已验证单协议脚本。缺能力时允许在**测试 worktree** 最小修订工具：支持 `--workers 8`、绝对 client binary、同步测量窗口、结果判定和 GB/HTTP-FLV 混合路径；变更前后跑 smoke，记录工具 diff 和二进制 SHA。**定位性能瓶颈时也允许在测试 worktree 修改业务代码进行验证**，但必须遵守第 9.5 节的实验与 A/B 规则；不得为方便而重写整个 benchmark 框架。

## 7. 执行流程与窗口

严格顺序：

**阶段 0：盘点**。固化 HEAD、构建、8 CPU 映射、客户端 CPU、fixture、namespace 网络、磁盘预算、工具；输出 `environment.txt`、`manifest.json`，所有预检通过才开始。

**阶段 1：Smoke**。以每种重点输入/输出的 1 source × 1 viewer 检验协议、音视频和资源清理。建议 warmup 5–10 秒，测量 15 秒，一次。任一关键 case 失败先诊断，不带病进入容量搜索。

**阶段 2：正常基准**。对每个重点输出协议测试空载、publish-only、1、4、8、16、100 路等低中负载，**仅保留实际通过且工具可承受的档位**。每档：15 秒 warmup + 60 秒测量、3 次**独立**重复，统计 median/min/max、离散程度。服务端每轮重新启动，所有进程停止后才重测。实际大负载按能力增加到 500、1000 等；数值应记录为配置而不是硬编码。基准期间不开重型 perf/eBPF/jemalloc profiling。

**阶段 3：健康容量搜索（100 路粒度）**。

1. 前提：同一 profile、8 CPU、8 workers、同一 source 数、同一协议、固定客户端 CPU 组，已有可用小档位。
2. 从已验证的健康点 `H` 出发，**每次增加 500 路**（例如 500→1000→1500→2000）；预探索允许较短 warmup/measurement，但每个结果仍必须用完整健康判断，不可只看建立数。
3. 第一次看到可信失败档 `F` 时，停止增加负载。若失败由客户端、磁盘、端口、资源不足、工具故障引起则标 BLOCKED，不算 `F`。
4. 在 `[H,F]` 之间按**100 路为最小粒度**二分，测试 `M = floor(((H + F)/2)/100) × 100`；要求 `H < M < F`。`M` 健康则令 `H=M`，失败则令 `F=M`。当 `F-H <= 100` 停止。起始 H、F 均选择 100 的倍数，避免边界模糊。
5. 最后对 `H` 做至少 **3 次** `30 秒 warmup + 90 秒测量` 正式独立复验，对 `F` 至少 **2 次**同条件复验；不稳定/结果不单调则标 `INCONCLUSIVE`、扩大复验，不能宣称精确最大容量。
6. 结论写为“**当前资源和媒体条件下，100 路粒度的最高已验证健康档 H，最低确认失败档 F**”，不是数学意义的最大连接上限，不对其他编码/网络条件泛化。
7. 记录建连速率（ramp/s），预热必须从全部目标会话 ready 后计时；若 pre-measurement 失败则仍为该档失败，不能缩小分母只统计成功者。

**阶段 4：过载诊断**。以已确认的 `H/F` 为基准，依次尝试 `max(F, ceil(1.1×H/100)×100)`、`ceil(1.25×H/100)×100`、`ceil(1.5×H/100)×100`；有充分系统余量且可诊断才考虑 `2.0×H`。每档目标是揭示首个明确瓶颈/队列增长机制，通常短采样 15–30 秒即可。**必须先分析接近失败边界的档位，再尝试严重超载**。过载档不用于常规吞吐排名。

**阶段 5：长时验证**。选重点健康负载运行 30–60 分钟，观察 PSS、FD、队列、源更替、自然关键帧以及关闭回收。不能用短时容量通过代替长期稳定性通过。

### 7.1 测量窗口与健康标准

- 统一使用 monotonic duration 测量；server 及全部 client 的统计窗口必须重合。事件时间如为 UNIX ns，记录并核对起止偏差，不直接平均不同窗口的 CPU 和吞吐。
- `expected == established == media_ready == progressing`（根据协议含义调整）；整个窗口内音/视频持续增长。无音频 profile 只验证视频。首媒体延迟和建连失败单独统计。
- 无非预期断连、输入故障、音视频时间戳倒退、RTP 未预期 sequence gap、WHEP SRTP unprotect 失败、超限 write queue、UDP drops。只根据协议支持的可观测数据判断，不凭空宣称某指标已测。
- 进程退出、连接关闭、source/session registry、FD、端口和 PSS 清理必须检查。客户端进程本身耗尽资源、drop 包或未按速率消费时另行归因。
- 允许的 HLS 分片时延、RTP 丢包注入等专项容差必须预先明确；不临时降低 PASS 门槛。**持续正确处理才是健康，不只是进程存活。**

### 7.2 资源与停止条件

预检时根据机器和测试预算确定 `MAX_CLIENTS`、`MAX_RSS`、`MAX_PSS`、`MAX_DISK_BYTES`、`MAX_DURATION` 等值，写入 manifest；规范不擅自替本机假设安全数值。每秒检查服务端、客户端、整机可用内存和 PSI，监控输出磁盘空间。

触发以下任一项立刻停止当前压力增加：潜在 OOM、内存持续异常增长、CPU/memory PSI 明显恶化且无法继续诊断、磁盘/日志可能写满、测试客户端失去发包/收包能力、测试服务崩溃、端口/FD/临时文件资源耗尽。必要时终止当前档位，保留证据，执行清理。不能通过只增加日志级别和扩大日志存储来“解决”服务器故障。

## 8. 性能观测（轻量指标必须每档采集）

| 维度 | 需要记录的事实 |
|---|---|
| 环境 | Git HEAD + dirty diff、build SHA、compiler/flags、依赖版本、fixture SHA、CPU/NUMA/SMT、namespace IP、内核、机器负载 |
| 服务器 CPU | 进程 CPU seconds / wall seconds（即平均 CPU cores）；每线程 CPU、上下文切换、迁移、run queue、频率 |
| 内存 | RSS、PSS（`/proc/<pid>/smaps_rollup`）、FD、线程数；分初始/稳态/峰值/结束/清理后；不得将 VSZ 视为已使用物理内存 |
| 网络 | per-protocol 收发字节、packets/s、TCP retrans、UDP drops、socket rx/tx queue；namespace `lo`/socket 计数 |
| 媒体 | ready/progressing、音频/视频 bytes/packets、接收缺口、解密失败、HLS 请求数/分片数、首媒体耗时 |
| 系统 | `/proc/pressure/{cpu,memory,io}`、物理核心 CPU 使用、机器可用内存、磁盘空间 |
| 负载端 | publisher 与 client CPU/PSS、实际发包频率、失败数、流量与丢包；用于判定 `BLOCKED_CLIENT` |

采样可沿用 `bench/fanout_support.py::proc_snapshot`；1 秒一次一般足够。注意 `smaps_rollup`、系统 tracing 有一定开销，正式 A/B 要相同采样设置。为 TCP/UDP 指标固定采样来源和 delta 计算方式，不能将全机 netns 计数不加归因当成某一个协议的独立计数。

报告中建议至少展示：`server_cpu_cores`、`server_pss_mib_median/peak`、`server_rss_mib_median/peak`、`aggregate_mbps`、`ready/progressing/failed`、`queue_full`、`udp_drops`、`client_cpu_cores`、`first_media_p50/p95/p99_ms`、`status`。

## 9. On-CPU、Off-CPU、jemalloc：专项诊断必须做

诊断需要分别回答：“CPU 花在哪里”、“线程没运行时卡在哪里”、“内存是哪些调用栈分配/保留的”。**三者互补，不允许只看一个火焰图就下结论。**

### 9.1 On-CPU

首先在**刚开始失稳**档附加采样，再选择明显超载档对照。查 `perf list`、权限、内核符号和二进制调试信息，保持 CPU/worker 固定。示例：

```bash
PID=$(cat "$RUN_TMP/server.pid")
test -d "/proc/$PID"
perf stat -p "$PID" -e task-clock,cycles,instructions,cache-misses,context-switches \
  -o "$RUN_TMP/perf-stat.txt" -- sleep 20
perf record -F 99 -g -p "$PID" -o "$RUN_TMP/oncpu-perf.data" -- sleep 20
perf report --stdio -i "$RUN_TMP/oncpu-perf.data" > "$RUN_TMP/oncpu-report.txt"
```

以上为**诊断运行**，不能取代未插桩的正式基准结果。硬件事件不支持或 perf 权限不足时，记录精确错误、改用可用的软件事件或标 `BLOCKED`。若缺帧指针，考虑 DWARF call graph（开销更大，单独标记）。分析热点调用栈、用户态/内核态比例、时间占比、各 worker CPU，而不是只列函数名。

### 9.2 Off-CPU

目标：识别 worker 等待、锁争用、epoll/scheduler、TCP backpressure 或 IO 等等待源；**正常 idle 的 `epoll_wait` 不自动构成性能问题**。优先使用内核适配的 BCC `offcputime-bpfcc` / `perf sched`，版本不同先看 `--help`；建议：

```bash
PID=$(cat "$RUN_TMP/server.pid")
command -v offcputime-bpfcc && offcputime-bpfcc --help
# 确认当前工具的 -p 参数确实按进程过滤后，示例：
offcputime-bpfcc -p "$PID" -f 20 > "$RUN_TMP/offcpu-folded.txt"
```

如可运行 `perf sched`，则保存 sched trace 与 `timehist`，并以服务器的 PID/TID 过滤，证明线程何时 runnable、何时实际运行。注意 BCC/eBPF 内核兼容、stack symbols 和进程过滤语义；缺失信息标 `BLOCKED`，不能把全机采样误认为单进程。

### 9.3 jemalloc 堆和保留内存

先检查 `ldd <media_server>`、`/proc/$PID/maps`、jemalloc 版本、配置和 profiling 是否启用。**不能默认仅因为链接了 jemalloc 就能 dump heap，也不能随意给现有服务 `LD_PRELOAD` 替换 allocator。**

当 jemalloc 编译了 `--enable-prof` 时，可以在**独立诊断运行**给服务器 wrapper 设置 `MALLOC_CONF`，例如：

```bash
# 必须先确认 jemalloc 支持 prof / prof_gdump；仅给 media_server 进程：
BENCH_MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:19,prof_gdump:true,prof_prefix:${RUN_TMP}/jeprof"
export BENCH_MALLOC_CONF
# wrapper 仅对 media_server 导出 MALLOC_CONF；然后重启服务器复现增长。
```

按第 4.1 节 wrapper 注入到服务器，**不要直接 `export MALLOC_CONF` 到整套 runner/FFmpeg/client**。确认诊断结束实际生成了有效的 heap dump；有 `jeprof` 时使用其对应版本处理并保存按 bytes 和 allocations 排序的调用栈报告。区分**live allocations、active pages、resident、retained** 及 PSS，比较稳态前、中、后堆数据；不能从 RSS 未下降单独推断内存泄漏。

如果当前 jemalloc 没有 profiling 支持，记录 `BLOCKED`，由后续专门任务构建**可比较的诊断版**，不在正式基准中临时更换 allocator。

### 9.4 诊断报告的最低要求

每个确认瓶颈的 case 至少交付：①最后健康 H 与首次失败 F 的 CPU/PSS/媒体/队列对比；②On-CPU 热点调用栈及比例；③Off-CPU 等待分类；④jemalloc 分配/保留内存证据或明确 BLOCKED；⑤排除客户端、磁盘和 namespace/network 工具瓶颈的证据；⑥最小优化候选，附预期受影响路径和可验证 A/B 指标。

**先观测，再定位，再独立优化**。严禁看到热点就顺便增设新的锁、状态字段、buffer、重试或广义架构层。

### 9.5 性能热点验证与实验性代码修改（允许，但必须可比较）

允许 Pi Agent 在**独立测试 worktree** 中直接修改 `media/`、`bench/` 或测试代码，以验证已由 On-CPU、Off-CPU、jemalloc 证据提出的性能假设；无需为每一次小实验回到主工作区。修改权限不等于可以无证据重构，也不等于批准把实验代码交付生产。执行规则如下：

1. **冻结 A（基线）**：先从干净 `BASE_HEAD` 构建 `baseline`，记录完整 CMake 参数、ELF SHA256、原始健康容量 H/F 和 profiler 证据；基线运行所用 Python runner/媒体 fixture 也保存哈希。不能先修改代码再事后声称测过基线。
2. **单一假设、最小修改**：一次实验只验证一个可描述的瓶颈，例如某个调用栈的重复编码、队列分配或锁等待。允许增加临时计数和日志，但诊断插桩版与正式 A/B 优化版必须分开；**带诊断日志或重型 profiler 的结果不用于吞吐/CPU 性能排名**。
3. **记录 B（实验）**：修改仅发生在 `$TEST_WORKTREE`，用单独构建目录编译；记录 `git diff --binary`、新增测试/文件清单、编译选项和实验 ELF SHA256。不得覆盖 A 二进制或覆盖历史 `result.json`。若实验引入测试工具变化，必须证明 A、B 使用一致的工作负载与判定标准。
4. **严格 A/B**：同一机器、namespace 拓扑、8 核 CPU ID、`--threads 8`、客户端 CPU、fixture SHA256、sources/viewers、ramp、warmup、measurement、采样设置；A 与 B 依次单独启动，避免相互竞争。每个关键档位至少 3 次独立运行，推荐交错 `A→B→A→B→A→B` 复验减少时间/温度漂移；记录失败率、CPU、PSS、媒体健康、客户端压力和离散程度。不得以降低观众数、降低码率或放宽健康标准换取“优化”。
5. **优先在问题拐点比较**：至少对比最后健康 H、首次失败 F 附近的相同固定观众数。优化若使 F 变健康，需按原搜索规则重新寻找并验证新的 H/F；容量提升不能由单次超载成功推断。
6. **失败也要留证据**：结果可能是有效改善、无改善、退化或 `INCONCLUSIVE`；均归档修改原因、实际 diff、诊断证据与 A/B 原始结果。只在有因果证据时提出最小生产优化候选，不直接修改主工作区、不自动 commit、push、merge 或 cherry-pick。

每轮至少保留以下差异证据（先审核脱敏，注意 `git diff` **不含未跟踪新文件**）：

```bash
cd "$TEST_WORKTREE"
git diff --check
git diff --binary > "$RUN_TMP/experiment.patch"
git status --short --untracked-files=all > "$RUN_TMP/experiment-status.txt"
sha256sum "$RUN_TMP/experiment.patch" > "$RUN_TMP/experiment-patch.sha256"
# 如有新源文件/测试脚本，逐个检查并单独归档，不要使用 git add -A。
# 脱敏检查后，将 patch、状态清单与必要新文件复制到 REPORT_DIR/experiments/。
```

若实验提出有效修复，在 DEV 的实验 worktree 修改后重新打包 B 并部署至 TEST 独立 runtime-B 目录进行 A/B；优先在本轮报告中提供可应用的补丁和明确的验证数据，由后续单独开发任务决定是否合入。不要让 Pi 在运行性能基准时同时改变被测实现；各 run 必须明确标记 `BASELINE`、`DIAGNOSTIC` 或 `EXPERIMENT`，并引用对应的代码和二进制哈希。

### 9.6 跨机清理与实验 worktree 收尾

先在 **TEST root** 终止本轮所有进程、清理本轮 namespace；将 TEST 运行目录的报告、原始失败证据、A/B 结果回传到 **DEV** 的 `$REPORT_DIR` 并核验完整性。之后仅删除 TEST 本轮 `<run_id>` 运行包及临时资料；不可清理其他 root 进程。再在 DEV 归档实验 patch 和新增文件，最后处理 DEV worktree：

```bash
git -C "$TEST_WORKTREE" status --short --untracked-files=all
# 仅对本轮创建且经核实没有其他 Agent 使用的临时 worktree 清理。
# 若存在未归档的修改或陌生文件，停止清理并标记 BLOCKED_CLEANUP。
# 只有所有实验修改都已归档、确认无其他使用者时，才恢复本轮改动并移除 worktree。
# git -C "$TEST_WORKTREE" restore --worktree -- .
# git -C "$TEST_WORKTREE" clean -nd                 # 仅预览
# git -C "$TEST_WORKTREE" clean -fd                 # 仅处理本轮已确认文件
# git -C "$MAIN_REPO" worktree remove "$TEST_WORKTREE"
# git -C "$MAIN_REPO" worktree list
```

**上述清理命令默认是注释，必须逐项核对后才执行**，不能机械调用 `git reset --hard`、`git clean -fdx`、`git worktree remove --force` 或全局 `pkill`。若 test worktree 有无法确认归属的文件，**宁可保留 workspace 并报告 `BLOCKED_CLEANUP`，也不删除可能属于他人的修改**。移除工作树后再删除 `$RUN_TMP` 的本轮临时资料；主工作区的报告目录和固定 fixture 必须保留，并验证 `git status` 未跟踪或暂存报告内容。

## 10. 每轮 Agent 操作清单

以下是强制执行门槛。不要跳过前阶段直接开始高负载：

```text
[ ] 阅读本规范、AGENTS.md 和最新 docs 中相关取舍
[ ] 记录主工作区 HEAD/dirty；创建独立 TEST_WORKTREE 与唯一 RUN_ID，不改动主工作区
[ ] 确认仓库 HEAD、dirty 状态、构建 hash；不得覆盖用户修改
[ ] DEV 已以 gyl 创建独立 worktree，核实 GCC16 / Boost1.92 / FFmpeg901 静态 SDK 与构建 SHA
[ ] DEV 通过 bench/package_runtime.py 打包，传送到 TEST=root@172.20.63.32 并校验 tar SHA
[ ] TEST root 验证库依赖/运行包/fixture，TEST 环境单独归档
[ ] 在 TEST 实测硬件拓扑，固定 8 个物理 core ID + 8 workers
[ ] 验证客户端核心不重叠，并记录 SMT sibling / NUMA
[ ] 确认 fixture 文件、codec、ffprobe 与 SHA256
[ ] 创建唯一 network namespace，检查 lo 和两 IP
[ ] 验证 DUT 所有监听地址均在 namespace 内
[ ] 确认测试客户端、publisher 也在 namespace 内
[ ] 确认 docs/performance_reports/ 已被 Git 忽略
[ ] 先运行 1 source × 1 viewer smoke 并判定媒体健康
[ ] 开始 3 次独立正常基准，保存独立原始记录
[ ] +500 扩容、100 路粒度二分、复验 H/F；明确客户端瓶颈
[ ] 110% / 125% / 150% 分级超载，执行事前停止条件
[ ] On-CPU + Off-CPU + jemalloc 专项证据（或明确 BLOCKED）
[ ] 如修改业务代码，仅在 TEST_WORKTREE 完成，保存 diff/新文件/构建 SHA，执行同条件 A/B
[ ] 保存各协议矩阵的 PASS/FAIL/BLOCKED/UNSUPPORTED/NOT_RUN
[ ] 输出性能图表或表格、瓶颈分析、根因置信度和优化候选
[ ] 结束所有进程，核对无遗留 PID、端口或 namespace
[ ] TEST 数据通过 SSH/SCP 回传 DEV，核对 SHA256 后脱敏归档到 DEV docs/performance_reports/<run_id>/
[ ] 验证归档可读、路径正确、SHA256 完整
[ ] 确认所有修改及新文件已归档，先清理 TEST 本轮运行包，再移除 DEV 独立 worktree，清理两机各自 /var/tmp 本轮临时文件
[ ] 检查主工作区 Git status、测试 worktree 最终状态记录，确认报告、密钥、临时文件未被追踪/暂存
[ ] 最终报告列出未完成项；无自动 commit / push
```

## 11. 报告模板（写入 README.md）

每轮报告至少写清楚：

```markdown
# media_server 性能报告：<run_id>

## 结论
- 整体：PASS / PARTIAL / FAIL / BLOCKED
- 版本：MAIN_REPO HEAD/dirty、TEST_WORKTREE BASE_HEAD、A/B 构建 sha256、8 CPU ID、workers=8
- 媒体：fixture SHA256、codec、bitrate、fps、audio
- 环境：DEV/TEST 的地址、实际执行职责、TEST root 验证、运行包 SHA、TEST network namespace（是否已删除）、内核、NUMA
- 各协议健康容量：H / F / 100 路粒度 / 复验次数
- 首个明确瓶颈及证据（未证实时写“待确认”）

## 测试矩阵与执行记录
- 已运行、失败、阻塞、不支持、未运行的 case
- 各档位/各重复的命令、测量窗口、进程 CPU/PSS/吞吐/媒体健康
- 客户端是否饱和及其证据

## 异常与性能热点
- 正常基准与刚失稳档对比
- On-CPU 热点（调用栈及百分比）
- Off-CPU 等待（调用栈和等待时长；说明正常 idle）
- jemalloc heap（或不可用原因）
- 队列、PSI、TCP/UDP 统计

## 优化候选与验证计划
- 只列已受证据支持的问题；无证据不建议修改
- A/B：BASELINE/EXPERIMENT 代码 SHA、ELF SHA、同条件数据和实验 patch
- 推荐最小修改点、预期指标、A/B 测试方式

## 清理与边界
- 进程/namespace/临时文件/worktree 清理结果
- Git status 与报告忽略验证
- 工具、硬件或环境导致的限制和 NOT_RUN 项
```

## 12. 明确的已知边界

- **双机测试必须如实归属**：DEV 只编译/打包/部署/存报告，TEST 才是 8 核正式测试与 profiler 结果来源。SSH 不通即 `BLOCKED_REMOTE_ACCESS`；DEV 上跑出的测试不能替代 TEST。
- **独立工作区与跨机运行包是两件事**：DEV 的 Git worktree 可修改源码；TEST 使用有版本标识的 bundle，不需复制完整 Git 仓库，不混用 A/B ELF 或动态依赖。
- **运行包不是全静态**：必须经 `run.sh` 的 LD_LIBRARY_PATH 执行并检查动态库，禁止原样 SCP 单一 ELF 并在 TEST 上静默使用其他版本的 FFmpeg/Jemalloc/标准 C++ 运行库。

- **本机 namespace 不是跨机真实 NIC 容量测试**：所有流量经内核 loopback，不能直接外推实际网卡最大带宽、真实 RTT 或跨机 UDP 丢包容量。
- **根权限不等于可以改一切设置**：测试机可安装工具，但不改变全局配置后拿结果与旧基准直接比较。
- **8 workers 不等于拥有独立的 8 个物理核心**：必须实际 pin 且验证 sibling；客户端、软中断和内核线程仍可能造成影响。
- **GB28181、WHEP、HLS 使用不同成本模型**：GB 共享 PS、WHEP 包含 SRTP 和音频派生、HLS 多请求/共享分片，不以单一“每连接 CPU”横向排名。
- **RTSP pull 真实摄像机测试与封闭 namespace 基准不同**：真实设备测试另存报告，不能混成同一固定输入矩阵数据。
- **允许在独立 worktree 中修改代码验证性能假设**；但实验结果必须与冻结的 A 基线按相同条件比较，不能直接成为生产提交。
- 任何模型执行不出某个实验，要精确保存错误、说明 `BLOCKED/NOT_RUN`，不要补写不存在的观测结果。
