# 环境与隔离

## 职责

DEV：`gyl@172.20.45.187`，源码 `/home/gyl/example/media_server`；GCC **16.0.1 experimental**（不是 16.2）、Boost 1.92 静态、FFmpeg SDK `/home/gyl/ffmpeg901`、CLI `/home/gyl/bin/ffmpeg`。DEV 结果只证明功能，不混入 TEST 性能基线。

TEST：`root@172.20.63.32`，历史 Xeon Silver 4216 / 约 93 GiB；每次以实际 `uname/lscpu/free` 为准。正式约定 server CPU 0–7、client 8–15、worker 8，隔离地址 `198.19.0.10/.11`。必须先确认 Pi 当前 namespace、CPU 和服务归属，不能占用或删除活动环境。

## 构建（DEV）

在准确、clean 的提交上执行，子模块只初始化锁定版本：

```bash
git rev-parse HEAD
git status --short --untracked-files=all
git submodule status
git submodule update --init --recursive
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig cmake -S . -B build-tooling \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-16 -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
  -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 -DBoost_USE_STATIC_LIBS=ON \
  -DBUILD_TESTING=ON -DMEDIA_SERVER_BUILD_BENCHMARKS=ON \
  -DMEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg
cmake --build build-tooling -j12
ctest --test-dir build-tooling -j8 --output-on-failure
```

不要误用 `BUILD_BENCHMARKS`：当前真实选项是 `MEDIA_SERVER_BUILD_BENCHMARKS`。显式选择已验证 FFmpeg CLI；系统旧 FFmpeg 可能不能写 enhanced HEVC FLV。RelWithDebInfo 使用 `-O2 -g -DNDEBUG`，项目带 `-Werror`。保留 CTest 已有资源锁，不把全部测试串行化。与其他实例冲突时使用自己的 namespace，而不是杀其他服务。

## 短时 DEV 隔离

支持非特权 user/network namespace 的环境可用以下命令创建自己的临时网络空间，不改宿主网络，也不占 Pi 的空间：

```bash
unshare -Urn sh -c 'ip link set lo up; ip address add 198.19.0.10/32 dev lo; ip address add 198.19.0.11/32 dev lo; exec bash'
```

在该 shell 内运行短时工具或 CTest，退出前关闭自己启动的程序。如果系统禁止 user namespace，则 BLOCKED，不绕过安全配置。小规模功能可以用 server CPU 0–3/client 4–7，不能与正式 TEST 数据比较。

## TEST 只读预检（Pi 执行）

```bash
hostname; uname -a; lscpu; free -h; df -h /tmp
cat /proc/self/limits
ip -br address
ss -lntup
cat /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io
command -v python3 perf offcputime-bpfcc jeprof ffprobe
python3 -c 'import aiohttp; print(aiohttp.__version__)'
```

实际运行之前还需核对 CPU affinity、媒体 UDP 池与客户端端口隔离、动态依赖、空间和已有进程。工具默认资源预算为 0，不执行阈值；Pi 必须根据当时环境设置 `--min-free-memory-mib`、`--min-free-disk-mib`、`--max-server-pss-mib`。不得为了成绩调系统参数。

仅清理本轮 PID/starttime 身份确认过的进程。无全局 kill、无全局 /tmp 清理、无其他 Agent worktree/namespace 操作。运行包不包含宿主 Python/glibc/Chrome/性能诊断内核能力；依赖缺失为 BLOCKED，详见打包说明。
