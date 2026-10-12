# perf_validate：授权、源停止与取消负对照

入口：`python3 bench/perf_validate.py --help`。目的不是获得吞吐，而是证明健康判据会拒绝真实故障且能清理自有资源。

## 参数和依赖

- `--output`：必填新目录，不覆盖。
- `--cases`：默认 `invalid_token,expired_token,replayed_token,source_stop,cancel`；必须是非重复已知名称。
- `--case-timeout`：默认180秒，正整数。
- `--` 后透传 perf_run 的服务、fixture、地址、CPU 参数；不得覆盖 output/input/outputs/sources/fault/duration/warmup/progress-interval/group-size。
- 依赖与自管 perf_run 相同；使用一次性 SignalingStub，不代表完整 Go token 生命周期 E2E。

每项固定 RTMP 输入、1源、FLV 2 viewer，每个 viewer 独立客户端进程（group-size1），warmup1、progress2。通常测18秒；取消场景请求60秒，在共同测量起点之后发送 SIGTERM。

```bash
python3 bench/perf_validate.py \
  --output /var/tmp/perf-negative-001 --case-timeout 180 -- \
  --host 127.0.0.10 --client-address 127.0.0.11 \
  --rtmp-port 45935 --rtsp-port 45954 --http-port 45980 \
  --server-cpus 0-3 --client-cpus 4-7 --workers 4
```

先确认地址、CPU、端口和运行包依赖，不能与其他 Agent 的媒体实例共用地址/端口。只验一项时使用例如 `--cases source_stop`。

## 判据

| 场景 | 故障实施与必须的真实证据 |
|---|---|
| invalid_token | 随机未签发 token；每个客户端均有授权拒绝及 ready=0，不能只用一个403或非零退出判PASS |
| expired_token | fixture 验证服务签发 TTL0 的凭据；每个客户端实际被拒绝 |
| replayed_token | 先真实请求 verify 消耗一次，再让客户端使用同 token；后续必须实际拒绝 |
| source_stop | 所有客户端先双轨ready，测量中点异常kill本轮publisher（5秒有界等待）；result 保存请求/实际完成单调时间和目标退出，实际完成必须在窗口内，所有客户端报告测量后的媒体失败 |
| cancel | 等待 measurement barrier 后 SIGTERM；runner 保存 KeyboardInterrupt 和清理结果，而不是在启动前退出就算成功 |

源停止的启动期 connect error 不是成功负对照。部分客户端正常播放、另一个被拒绝也不是整体成功。预期故障不等于 SERVER_UNSTABLE/CLIENT_RESOURCE；这些资源边界使本项 INCONCLUSIVE，不能用于容量搜索。

## 输出与清理

根 `result.json` 包含逐项状态、原因、runner退出码、原始result及日志路径。子目录保留对应 perf_run result、凭据和原始日志；`CASE.runner.log` 保留驱动输出。

负对照 PASS 的 runner 通常非零退出，这是预期故障；**负对照汇总自身**必须退出0且所有场景PASS。缺result、未完成取消、残留 owned process/receiver 或 cleanup.errors 都不能PASS。

超时/取消只针对本项启动的进程组；SIGTERM 后最多30秒、必要时SIGKILL。执行非交互 stdin=DEVNULL，FFmpeg保留-nostdin。不会创建 namespace、修改sysctl、停止外部服务。

原始 token/URL 和日志敏感，受限保存、不得直接公开提交。结果仅为功能/负对照验证，不是正式性能结果。CLI/offline：DEV VERIFIED；实际媒体验收以本轮记录为准；TEST正式执行：REQUIRES_TEST。

参见 [perf_run](perf_run.md)、[结果格式](result_format.md)、[Pi手册](pi_handoff.md)。
