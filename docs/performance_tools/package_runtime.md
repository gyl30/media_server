# `package_runtime.py`：DEV → TEST 运行包

用途是将已构建媒体服务、正式性能客户端、Python 工具、媒体素材及非系统动态库组成 runtime-only 目录。它不编译源码、不生成 fixture、不创建 namespace、不启动服务、不安装 Python/浏览器/诊断工具。

## 构建与输入

在 DEV **本轮独立 worktree 根目录**执行。先完成对应 HEAD 的独立构建并核对 `CMakeCache.txt`、产物 SHA；不得依赖他人活动 build。需要 Python 3.10+、Git、ldd、FFmpeg/ffprobe CLI、实际 fixture、8 个已构建 ELF，以及本机 NSS 插件目录 `/usr/lib/x86_64-linux-gnu/nss`。

| 参数 | 含义 |
|---|---|
| `--output` | 必填，新目录；已存在时拒绝覆盖 |
| `--fixture` | 必填，固定常规输入 fixture；输出名 `.cache/perf_fixture_720p30.flv`，文件名并不验证实际 codec/分辨率 |
| `--build-dir` | 默认 `build`，含以下正式 ELF；存在 CMakeCache 时同时记录并复制 |
| `--ffmpeg-bin`、`--ffprobe-bin` | 默认 `/home/gyl/bin/ffmpeg`、`/home/gyl/bin/ffprobe`；使用准确已验证 CLI，不是 FFmpeg SDK |
| `--whip-fixture` | 可选，真实 H.264/Opus 离线 fixture，复制为 `.cache/perf_fixture_whip.mkv` |
| `--signaling-bin` | 可选，已构建 Go signaling ELF，复制为 `bin/signaling`；支持静态 ELF |

## 最小打包与传输

下列路径必须指向自己本轮的构建与媒体文件。`TEST_RUN` 必须事先核实不存在，由 Pi 在独立部署时创建；不使用其现有 namespace 或覆盖历史结果。

```bash
python3 bench/package_runtime.py --help
export BUILD_DIR=/var/tmp/media_server_tooling_acceptance/build
export FIXTURE=/var/tmp/media_server_tooling_acceptance/assets/fixture.flv # 本轮实际使用；另轮换为已核验素材
export BUNDLE_PARENT=$(mktemp -d /var/tmp/media-server-runtime-XXXXXX)
python3 bench/package_runtime.py \
  --build-dir "$BUILD_DIR" --fixture "$FIXTURE" \
  --ffmpeg-bin /home/gyl/bin/ffmpeg --ffprobe-bin /home/gyl/bin/ffprobe \
  --output "$BUNDLE_PARENT/runtime"
tar -C "$BUNDLE_PARENT" -czf "$BUNDLE_PARENT/runtime.tar.gz" runtime
(cd "$BUNDLE_PARENT" && sha256sum runtime.tar.gz > runtime.tar.gz.sha256)
# 根据真实验收需求，在打包命令追加 --whip-fixture 和 --signaling-bin，不能假定可选资产已存在。
```

```bash
export TEST_RUN=/var/tmp/media_server_perf/my-unique-run # 替换为本轮唯一目录
ssh root@172.20.63.32 "test ! -e '$TEST_RUN' && mkdir -m 700 '$TEST_RUN'"
scp "$BUNDLE_PARENT/runtime.tar.gz" "$BUNDLE_PARENT/runtime.tar.gz.sha256" "root@172.20.63.32:$TEST_RUN/"
ssh root@172.20.63.32 "cd '$TEST_RUN' && sha256sum -c runtime.tar.gz.sha256 && tar -xzf runtime.tar.gz"
```

禁止跳过传输 SHA。TEST glibc/loader 仍需兼容，运行包不是完全静态容器；解包后检查 `./run.sh ldd build/media_server` 以及各客户端是否存在 `not found`。

## 实际资产与 manifest

固定 ELF：`media_server`、`whep_fanout`、`whip_publisher`、`rtmp_fanout`、`rtsp_fanout`、`http_flv_fanout`、`rtsp_publisher`、`gb_ps_fanout`。

同时包含 `bench/*.py`（不含打包器自身）、`bench/signaling_stub.py`、`bench/requirements.txt`、FFmpeg/ffprobe、素材、ldd 非系统闭包及 NSS 模块。`run.sh` cd 到 bundle 并设置 `LD_LIBRARY_PATH`、`MEDIA_SERVER_BENCH_HEAD` 后 exec 用户指定命令；不会自行选负载或启动服務。

`manifest.json`：

- `head/dirty/worktree_status/submodules` 为**打包时源码工作区**事实；不是自动证明每个 ELF 对应该提交。
- `build.binary_source_head=NOT_RECORDED` 明确二进制编译提交未由 ELF 独立记录；Pi 必须将准确 HEAD 的干净构建命令、CMake flags 与 ELF SHA 保存到外部证据目录。
- `build.cmake_cache` 和打包的 `build/CMakeCache.txt` 记录可核对的构建来源、编译器/flag/SDK 路径；这些是历史构建信息，不作为 TEST 动态搜索路径。
- `files` 覆盖所有实际复制文件和 `run.sh` SHA；`libraries/nss_modules` 记录源路径及 SHA；fixture 有独立 SHA。
- `tool_versions` 记录所选 FFmpeg/ffprobe 的 `-version` 输出及退出码；非零不能冒充已验证工具版本。
- `host_requirements/optional_assets` 明确 Python、glibc 及未内置专项资产。

可在解包的 runtime 根目录直接验证 manifest（不需要原 DEV 仓库）：

```bash
python3 - <<'PY'
import hashlib,json,pathlib
root=pathlib.Path('.')
m=json.loads((root/'manifest.json').read_text())
expected=dict(m['files'])
expected.update({'lib/'+k:v['sha256'] for k,v in m['libraries'].items()})
expected.update({'lib/nss/'+k:v['sha256'] for k,v in m['nss_modules'].items()})
for name,want in expected.items():
    h=hashlib.sha256()
    with (root/name).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''): h.update(block)
    if h.hexdigest()!=want: raise SystemExit('SHA mismatch: '+name)
print('manifest SHA verified:',len(expected),'files')
PY
./run.sh python3 bench/perf_run.py --help
./run.sh python3 bench/perf_capacity.py --help
./run.sh python3 bench/perf_profile.py --help
```

## 依赖边界与失败

未内置：系统 Python、aiohttp 实际安装、Chrome/Firefox、Playwright、GB simulator、perf/BCC/jeprof、旧 lifecycle disconnect 测试资产/编译器。正式统一负载使用 native 工具和 Python；浏览器、真实 SIP 模拟器、诊断模式按实际任务补齐依赖并记录 SHA/版本。复制脚本不等于脚本全部功能无依赖可用。

打包器对缺输入、缺 NSS 模块、ldd 缺库/失败、同 SONAME 不同内容等直接非零失败；不能把半成品当 PASS。已有目录拒绝覆盖，失败产物先保留用于复核；不会自动删除其他文件。Python requirements 是依赖清单，不是在包内自动 vendoring，缺依赖应明确 BLOCKED/失败，不静默使用别处 SDK。

正常打包只创建指定新目录，不启动进程或网络服务；归档与解包的存储预算由调用方预检。manifest、CMakeCache、原始 URL/日志可能包含路径或凭据，归档应受限保存，未经脱敏不得公开提交。压缩包 SHA 不能替代解包后逐项文件检查。

离线 acceptance 测试覆盖静态 ELF、manifest 启动器和构建来源；真实 runtime 启动、TEST 部署与正式测量分别需要实际运行证据，不能仅依据打包成功标为 TEST VERIFIED。
