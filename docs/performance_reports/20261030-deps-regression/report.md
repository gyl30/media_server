# 依赖重组验收、缺陷修复与全协议回归（2026-10-30）

- BASE_HEAD：`1d5cfc7`。被测最终二进制为 `93716e3` 的源码与链接配置（启动日志版本串为构建时的 `63f180d-dirty`）；本报告随后单独提交。全部提交仅在本地 main，未推送、未部署。
- 性能基线：rt-gk0，即 [20261029-aac-whep-capacity](../20261029-aac-whep-capacity/report.md) 使用的 `29a7d82` 构建（动态链接系统 OpenSSL 3.0.2 与 jemalloc 5.3.0）。
- 测试机：root@<TEST_HOST>（Xeon Silver 4216，Ubuntu 22.04.5，glibc 2.35），netns `msperf-20261013T011210Z-7788320`。服务端固定 cpu 0-7（8 worker），客户端与上游使用 8-15、24-31，HT 兄弟 16-23 空闲；上游固定为 rt-e1。
- 本地只做构建与打包；2026-10-29 中途起所有运行与验证都在测试机上进行。此前在本机完成的控制 API、全协议矩阵、sanitizer 子集和 Go 信令兼容性结果在下文单独标注。

## 结论

- 依赖体系能在全新 checkout 中从零构建；依赖输入、编译器或路径不一致时 CMake 直接失败，不会混用系统库或过期产物。
- 本轮修复的确定缺陷：
  - 控制面 JSON 重复字段两侧解释不一致；
  - `service.cpp` 残留 Boost.Scope；
  - Opus 编码回退到 FFmpeg 实验编码器；
  - 测试客户端误判；
  - `perf_publish` 使用旧信令接口；
  - 服务端仍动态依赖 libgcc_s 的 `GCC_13.0.0`（Ubuntu 22.04 不带该版本，无法单独启动）；
  - jemalloc 5.4.0 的内存回归。
- 依赖选择：
  - OpenSSL 回到 3.5 LTS。WHEP 播放每会话 PSS 增加约 27 KiB，为接受的代价。
  - jemalloc 固定 5.3.0。5.4.0 的 RTSP 6000 路 PSS 从 1.8 GiB 升到 6.2 GiB，已二分到两个上游提交（第 4 节）。
  - 不修改 ireader `rtsp-muxer`：缩小其 1 MiB 内嵌缓冲在 5.3.0 下反而使 RTSP PSS 高约 330 MiB。
- 性能哨兵：9 组场景与基线相比 CPU 持平或更低。除 WHEP 播放（+14%，来自 OpenSSL）及其带来的混合场景 +4% 外，PSS 都在噪声内。实时比、UDP 丢包、FD 和控制延迟无退化。

## 1. 依赖与构建

| 检查项 | 结果 |
| --- | --- |
| 全新 checkout | 本机全新 clone 上 `deps/build.sh` 与 Release 构建一次通过，工作区保持干净 |
| 依赖变化后重建 | `BUILD-INFO` 记录 `sources.txt`、`build.sh` 的 sha256 与 C/C++ 编译器版本；任一不一致时 CMake 配置失败。子模块 HEAD 变化触发 FFmpeg 重建（`b062993`） |
| 旧构建目录复用 | 缓存的 OpenSSL、Boost、jemalloc 路径不在 `deps/install` 下时配置失败，避免链接 `/usr/local` 等处的旧库 |
| 中断与重跑 | sha256 失败、中断或出错时删除半成品；已有完整安装时拒绝覆盖 |
| sanitizer 依赖 | ASan、TSan 各用带注解的 ucontext 版 Boost.Context（`deps/install/asan`、`deps/install/tsan`），CMake 按模式核对路径；sanitizer 构建不链接 jemalloc |
| 冗余组件 | 删除 FFmpeg 原生 Opus 编码器及其回退（`788f011`） |
| 静态链接 | libstdc++、libgcc 与 sanitizer 运行时静态链接。GCC 16 下 CMake 把 C 隐式库 `gcc_s_asneeded` 显式追加到链接命令，使 `-static-libgcc` 失效，ELF 仍 NEEDED libgcc_s 并要求 `GCC_13.0.0`；已从 C 隐式库中移除，Release 与三种 sanitizer 构建只依赖 libc、libm（及 ld.so） |
| 构建身份 | `--version` 与启动日志首行输出 `git describe --dirty`、编译器、Boost、OpenSSL、FFmpeg、opus、libsrtp、分配器版本；ELF 带 build-id |

OpenSSL 选择 3.5 LTS（支持到 2030-04）。4.1 只支持到 2027 年，libsrtp 的 CI 也只覆盖 3.x，项目不依赖 4.x 的新能力。jemalloc 5.3.0 的 C++ 包装与 GCC 16 libstdc++ 不兼容（`__throw_bad_alloc`），改用 `--disable-cxx`；`operator new/delete` 仍经 malloc/free 落到 jemalloc。

## 2. 近期代码修改审查

- **simdjson**：
  - 原 Boost.JSON 与 Go `encoding/json` 对重复键取最后一个值，simdjson DOM 取第一个，同一请求可能被信令和媒体服务解释成不同内容。控制面改为拒绝重复字段并返回 400（`f04374c`）。
  - `regression/control_api.py` 覆盖非法 JSON、缺失/未知/重复字段、数值范围与溢出、编码与转义、空值、类型错误和状态码。本机 Release/ASan/UBSan/TSan 均 76/76 通过。
- **scope_exit**：自有实现覆盖原用法，无需引入 Boost.Scope；修复 `service.cpp` 残留的 Boost.Scope 依赖（`a1418b5`）。
- **libfdk-aac / libopus**：编码器缺失时启动直接失败并报错，不再回退到 FFmpeg 原生实验 Opus 编码器，也不放宽 `strict_std_compliance`（`788f011`）。
- **afterburner=0**：
  - 客观复测见 [README](README.md#aac-afterburner0-的证据2026-10-29-复核)：128 kbps 下 SNR 降低 0.8–7.9 dB，编码 CPU 为开启时的 42–56%，延迟不变（2048 样本）。
  - 主观证据只有一次非正式试听，没有作答记录或 ABX，只能说明未发现明显劣化。
- **DTLS（OpenSSL 3.5.9）**：
  - 自签名证书生成与握手由矩阵中的 WHIP、WHEP 播放、WHEP Pull 场景，以及哨兵中 2500 路 WHEP、400 路 WHEP Pull 覆盖，全部建立 SRTP 并实时交付。
  - 上一轮已用 WHEP Pull 验证 OpenSSL 3 与 4.x 两端互通。
- **Go 信令兼容性**（本机）：用真实 Go 信令签发的推流设备凭据做 RTMP、WHIP 推流后播放，均通过。

## 3. 全协议端到端回归

[`regression/protocol_matrix.py`](../../../regression/protocol_matrix.py)（`1d7bd27`）用 `bench/perf_run.py` 在回环地址上覆盖以下组合：

- RTMP、RTSP（TCP/UDP）、WHIP、RTMP/RTSP/WHEP Pull、GB UDP/TCP active/TCP passive 输入；
- H.264（含 B 帧）、H.265、AAC、Opus、G.711 A/U 编码；
- RTMP、RTSP、HTTP-FLV、HLS、WHEP 输出；
- 多源跨 worker、慢读者、源中断，以及非法和重放票据。

每个场景检查：

- 解码；
- 实时比；
- PTS/DTS 单调；
- AV 偏移；
- 控制面延迟；
- 清理后 FD、会话和进程回到基线。

H.265 WHEP 与 Opus GB 属于协议或实现不支持的组合，不列入。故障场景要求观察到预期的失败类别：perf_run 退出码为 1，矩阵判 PASS（UNCONFIRMED 表示客户端按预期未完成播放）。

| 运行 | 二进制 | 结果 |
| --- | --- | --- |
| 本机（指示前） | jemalloc 5.4.0 时的 Release | 31/31 PASS；实时比 0.999–1.001；AV 偏移恒定，最大为 WHEP Pull −32.6 ms；控制 p99 ≤2.02 ms；清理正常 |
| 本机（指示前） | ASan/UBSan/TSan 各 10 个场景 | 全部 PASS；ASan 只有已知的 makecontext 提示，UBSan/TSan 无报告 |
| 测试机 | Release（d3：5.3.0 + muxer 实验） | 31/31 PASS；Opus-in-FLV 源使用 FFmpeg 9.0.1 |
| 测试机 | ASan（d3）13 个场景 | 13/13 PASS；13 份 ASan 日志都只有 makecontext 提示 |
| 测试机 | 链接修复前的 HEAD：Release 31 个场景、ASan 13 个场景 | 44/44 PASS；ASan 日志只有 makecontext 提示 |
| 测试机 | 最终 Release | 31/31 PASS；不设 `LD_LIBRARY_PATH` 时 Release 与 ASan 二进制均可直接启动，`ldd` 只有 libc、libm、ld.so |
| 测试机 | 最终 ASan 13 个场景 | 13/13 PASS；15 份 ASan 日志都只有 makecontext 提示 |

修复的测试工具误判（`4fd9f2f`）：

- RTSP、GB、WHEP 客户端把 B 帧重排当作时间戳回退；
- WHEP 播放 offer 只带 Baseline profile；
- 精简 FFmpeg 不为 Matroska 视频推导 DTS；
- 进程退出时读取 `/proc/<pid>/fd` 返回 EACCES。

## 4. 性能哨兵

每组 ABBA 交替运行，各 2 轮；CPU 为服务端核数，PSS 为峰值 MiB。d2 为 `63f180d` 内容（jemalloc 5.3.0 静态）。

| 场景 | CPU d2 / gk0 | PSS d2 / gk0 |
| --- | --- | --- |
| RTMP 6000 | 5.25–5.26 / 5.25–5.26 | 1896–1904 / 1848–1862 |
| RTSP 6000 | 5.69–5.76 / 5.66–5.73 | 1735–1821 / 1729–1777 |
| HTTP-FLV 6000 | 5.65–5.70 / 5.64–5.67 | 1435–1479 / 1436–1439 |
| GB UDP sender 5000 | 5.23–5.28 / 5.21–5.25 | 84 / 81–82 |
| GB 输入 600 | 4.19–4.24 / 4.24–4.27 | 495 / 496–499 |
| WHIP 200 | 5.71–5.73 / 5.73–5.74 | 301–303 / 295–296 |
| WHEP Pull 400 | 5.87–5.88 / 5.97–5.99 | 556–561 / 555–556 |
| WHEP 播放 2500 | 5.49–5.51 / 5.58 | 534 / 470 |
| 混合 32 源（RTMP/RTSP/FLV 各 1600、WHEP 800、GB UDP 800） | 6.84–6.88 / 6.85–6.91 | 1461–1473 / 1407–1410 |

全部 36 轮 PASS HEALTHY：

- 实时比 ≥0.997；
- UDP 丢包、`queue_full`、`no_buffer_space` 均为 0；
- 所有观看者就绪并持续推进；
- FD 与观看者数一致。

WHEP Pull 400 路只作为当前哨兵；历史 496 路结论不作为当前 HEAD 的验收容量。

### 4.1 jemalloc 5.4.0 回归

`1d5cfc7` 的 jemalloc 5.4.0（d1）对比 gk0：

| 场景 | CPU | PSS |
| --- | --- | --- |
| RTSP 6000 | −5% | 1.76 → 6.2 GiB |
| RTMP 6000 | 持平 | +17% |
| FLV 6000 | 持平或略降 | +23% |
| WHEP 2500 | 持平或略降 | +13% |
| 混合 | 持平或略降 | 1.39 → 2.38 GiB |

二分方法：用 gk0 二进制加载 5.3.0..5.4.0 间各提交构建的 `libjemalloc.so.2`，共 557 个线性提交，在 RTSP 6000 路场景测量：

| jemalloc | PSS MiB | CPU |
| --- | --- | --- |
| 5.3.0 | 1844 | 5.65 |
| 5.4.0 | 6189 | 5.37 |
| `37bf846c`（`3c14707b` 的父提交） | 1782 | 5.60 |
| `3c14707b` | 2291 | 5.37 |
| `e350c715`（`a952a3b8` 的父提交） | 2381 | 5.37 |
| `a952a3b8` | 6353 | — |
| `a952a3b8` + `calloc_madvise_threshold:0` | 2259 | — |
| `5081c16b` 的父提交 / `5081c16b` | 1885 / 1733 | — |

- **主因 [`a952a3b8`](https://github.com/jemalloc/jemalloc/commit/a952a3b8b08a63609172c8c84cf6eb09de9fc7be)**（随 [PR #2864](https://github.com/jemalloc/jemalloc/pull/2864) 合入）：
  - 它把 `calloc_madvise_threshold` 默认值从 0 改为 8 MiB（同时启用 `experimental_tcache_gc`）。此后小于 8 MiB 的 calloc 不再请求已清零的 extent，而是拿到 extent 后整块 memset。
  - ireader `rtsp_muxer_t` 内嵌 `uint8_t ptr[1024 * 1024]`，每个 RTSP 播放会话 calloc 一次，6000 路因此多驻留约 4 GiB。
  - 选项本身由 [`5081c16b`](https://github.com/jemalloc/jemalloc/commit/5081c16bb49a0c9d1dde3cbd7dfb2e97c2827ea4) 加入，默认 0，不改变行为。
- **次因 [`3c14707b`](https://github.com/jemalloc/jemalloc/commit/3c14707b016b156c5f86dfd21304b01161c40750)**（[PR #2842](https://github.com/jemalloc/jemalloc/pull/2842)）：
  - 它把 dirty 大 extent 的合并限制在请求尺寸 << `lg_extent_max_active_fit` 以内，以提高复用、减少缺页。
  - 代价是 RTSP 约 +500 MiB、FLV 约 +20%（d1 加阈值 0 时 FLV 仍为 1748 MiB），CPU −6%。这是上游有意的取舍。
- **WHEP、混合场景**：d1 加阈值 0 时，WHEP 2500 为 530 MiB，与 d2 相同，增量与 jemalloc 无关；混合场景为 1494 MiB。
- **上游报告**：GitHub 未找到相同报告，两个 PR 的讨论也没有提到内存上涨。相关但机制不同的有：
  - [#2867](https://github.com/jemalloc/jemalloc/issues/2867)：5.3.0 元数据滞留；
  - [#2945](https://github.com/jemalloc/jemalloc/pull/2945)：结构布局回归；
  - [#3018](https://github.com/jemalloc/jemalloc/pull/3018)：SEC 清零正确性修复。

决定：固定 5.3.0（`63f180d`）。即使去掉 muxer 的 1 MiB 缓冲，5.4.0 仍会让 RTSP/FLV 多占约 20–30% 内存，只换来约 6% CPU。

### 4.2 缩小 rtsp-muxer 缓冲的实验（未采用）

d3 在 d2 基础上做了三处修改：

- 把 `ptr` 缩小为 3 × `rtp_sender_t::buffer`（6 KiB），只用于存放 SDP；
- TS/PS 分支改为按包 `malloc`/`free`（服务端不使用该分支）；
- 补上音频 SDP 的长度检查。

功能上 d3 通过完整矩阵与 ASan 子集。ABBA 结果：

| 场景 | CPU d2 / d3 | PSS d2 / d3 |
| --- | --- | --- |
| RTSP 6000 | 5.67–5.70 / 5.53–5.63 | 1752–1793 / 2077–2144 |
| WHEP 2500 | 5.53–5.58 / 5.49–5.52 | 534 / 526 |
| WHEP Pull 400 | 5.87 / 5.86–5.88 | 556–559 / 555–558 |
| 混合 32 | 6.80–6.81 / 6.80–6.82 | 1450–1462 / 1469–1505 |

用 `stats_interval` 在 RTSP 6000 路运行中周期输出 jemalloc 统计（复现 d2 1750、d3 2089 MiB），稳态对比：

| 项 | d2 | d3 |
| --- | --- | --- |
| 除 muxer 外的已分配 | 约 1220 MiB | 约 1220 MiB |
| muxer | 1.25 MiB 大块 × 6000，只有写过的页驻留 | 14 KiB 小块 × 6000，calloc 整块写零，82 MiB |
| dirty 页 | 约 330 MiB | 约 720 MiB |
| 统计窗口内 purge 页数 | 27.0M | 20.0M |

去掉 6000 个 1.25 MiB 大块后，相同分配流量下 jemalloc purge 减少约 25%，dirty 页多驻留约 390 MiB。缺页随之减少，CPU 低约 2.5%。在 5.3.0 下这一修改净增内存，而它针对的 5.4.0 calloc 问题已随版本固定消失。音频 SDP 越界读要求 SDP 片段超过 2 KiB，服务端只生成 AAC/Opus/G.711 的短 SDP，不可达。因此撤回，不修改 `third/`。

### 4.3 WHEP 播放 PSS（OpenSSL）

同一 gk0 二进制分别使用系统 OpenSSL 3.0.2 和 3.5.9 共享库（`LD_LIBRARY_PATH` 覆盖），WHEP 2500 路 ABBA：

| OpenSSL | CPU | PSS |
| --- | --- | --- |
| 3.0.2 | 5.58 / 5.62 | 469 / 471 |
| 3.5.9 | 5.50 / 5.50 | 536 / 536 |

+66 MiB（每个 DTLS 会话约 27 KiB）完全来自 OpenSSL 3.5，CPU 低约 2%；d2 的 534 MiB 与之一致。混合场景 800 路 WHEP 对应约 +21 MiB；其余约 30 MiB 与 RTMP 6000 的 +2% 同量级，未单独定位。为获得 LTS 支持周期接受该代价。

### 4.4 最终二进制

链接修复后的最终二进制与 d2 做 ABBA 对比：

| 场景 | CPU d2 / 最终 | PSS d2 / 最终 | 实时比最小值 |
| --- | --- | --- | --- |
| RTSP 6000 | 5.70–5.77 / 5.65–5.71 | 1696–1759 / 1744–1853 | ≥0.9993 |
| 混合 32 | 6.80–6.85 / 6.81–6.82 | 1452–1464 / 1447–1481 | ≥0.9992 |

两组差异都在此前 d2、gk0 各轮的波动范围内（RTSP 1729–1821 MiB），UDP 丢包为 0。

## 5. 文档与安全

- **敏感信息**：
  - 真实摄像机凭据在 Git 历史、已跟踪文件和工作区中 0 次命中，无需轮换。
  - 已推送的公开历史中有若干内网 IP（测试机、开发机），不是凭据。清除需要重写远端历史，未经授权未处理。
- **历史报告引用**：[README](README.md) 为每份报告标注被测版本、与当前代码的关系和缺失引用。
  - 报告目录此前未放行 `.diff/.cpp/.h`，a2-dispatch 的 B1/B2/B3 补丁和若干测试 harness 从未跟踪，已随本地清理丢失，已在导航中注明；现已补入白名单（`c3dc733`、`6433606`）。
  - AAC 报告中写错的提交号已修正。
- **afterburner 主观结论**：证据范围见第 2 节。

## 6. 未覆盖

- 真实摄像机、Firefox/Safari 和真实网络未在本轮验证。
- 最终二进制只做了 RTSP 6000 与混合场景的哨兵复核，其余 7 组沿用 d2 数据：两者源码相同，只差链接方式。
- 5.4.0 的 `experimental_tcache_gc` 未单独测量。阈值设为 0 后 RTSP 回到 5.3.0 + `3c14707b` 的水平，说明它不是主要因素。

## 提交

`a1418b5`、`b062993`、`788f011`、`f04374c`、`4fd9f2f`、`1d7bd27`、`c3dc733`、`9d85bf9`、`6433606`、`63f180d`、`93716e3`，以及本报告与文档更新。
