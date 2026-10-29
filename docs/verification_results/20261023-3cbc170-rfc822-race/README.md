# rfc822_datetime_format 数据竞争修复验证（2026-10-23）

- 基线：3cbc170。修复只改了 `third/ireader/libhttp/source/rfc822-datetime.c`：把 `gmtime()` 换成 `gmtime_r()`，结果写入局部 `struct tm`。
- 根因：`gmtime()` 返回 glibc 的进程级静态缓冲 `_tmbuf`。librtsp 每个应答都会经 `rtsp_server_reply2` 调用它，多个 worker 同时应答时会并发写这块缓冲，同时读它的字段。

## 目录

- `before-3cbc170/`：修复前的 release 和 TSan 二进制，sha256 见 `SHA256SUMS`。
- `harness/`：单元级复现。
  - `rfc822_check.c`：`race` 模式起 8 个线程，每个调用 20 万次；`equal` 模式与 3cbc170 实现逐字节比较。
  - `rfc822-datetime-3cbc170.c` 是原始实现；用 `build.sh <impl.c> <prefix>` 构建 tsan、asan、plain 三个版本。
- `scripts/rtsp_date.py`、`run_date.sh`：32 个连接持续发送 OPTIONS，持续 10 秒，校验每个 Date 头的格式、星期，以及时间是否落在请求/响应的时间窗口内。
- `scripts/regress.sh`：回归批次。生命周期和 perf_run 脚本复制自 `../20261023-04334df-final-acceptance`，`fixtures`、`sanitizer-runtime` 链接到该目录。
- `logs/`：构建日志和修复后的二进制 sha256（`SHA256SUMS-fix`）。
- `runs/`：原始结果。

## 结果

| 检查 | 修复前（3cbc170） | 修复后 |
|---|---|---|
| 单元 race（TSan，3 次） | 每次退出码 66，均报告 `_tmbuf` 竞争 | 每次退出码 0，0 份报告 |
| 单元 race（ASan+UBSan） | — | 退出码 0 |
| 单元 equal（plain/ASan/TSan） | — | 3758038 个时间点，0 处差异 |
| RTSP OPTIONS 并发（TSan，2 次） | 报告 24、23 次竞争，退出码 66 | 0 诊断，退出码 0 |
| RTSP OPTIONS 并发（ASan、UBSan、release） | release 无异常 | 0 诊断；Date 格式、星期、时间窗口全部正确 |
| 生命周期（TSan、ASan、UBSan、release） | TSan 退出码 66（`lc-tsan-3cbc170-b`） | 全部退出码 0，0 诊断 |
| perf_run RTSP TCP/UDP 推流输入、mixed（TSan、ASan、release） | — | 全部 PASS HEALTHY，0 诊断；清理后 fd 不高于基线 |

- 每次 RTSP 并发运行约 70 万个应答，全部为 200。
- 修复前后的 release 构建都没有出现格式错误的 Date 头：竞争窗口很短，而且字段大多相同。
- ASan + ucontext 每次都会写一条 makecontext 警告，所以 `report_files=1`，但诊断数是 0。
