# 测试发布者 TERM 超时的独立定位

2026-10-18，工作树`f567e4c713a6adacf18c88edb2e17b65d50f44b1`。本次只验证单个仓库外发布者的退出行为，不修改产品、公共bench、媒体、期限或健康门槛，不重复容量测试。

## 1. 方法与版本

原[安装版源替换观察](qt_monitor_client_stage7_source_cycle.md)的两份FAIL_CLEANUP保持不变。在DEV独立loopback27280–27284、fresh SQLite、两个media worker及独立设备下，以原Go/media ELF、原绿色H.264/AAC fixture和实际CLI重现：

```text
ffmpeg -nostdin -hide_banner -v warning -re -stream_loop -1 \
  -i <原fixture-c.mkv> -map 0:v:0 -map 0:a:0 -c copy -t 55 -f flv <独立授权>
```

保留`-nostdin`和stdin=DEVNULL。`-t 55`是输出媒体时长，不是55秒墙钟watchdog。strace跟踪自己启动的FFmpeg子进程，记录其exe、PID/starttime/PGID；媒体ready后发送一次TERM，原8秒期限不变，超时后保存线程瞬时状态，再发送第二次TERM，最多再等5秒。第二次信号属于诊断，不当作第一次优雅关闭通过；不能推广为产品或公共工具的新关闭契约。

CLI实际为`n7.1.1-57-g1b48158a23-20250824`、GCC15.1.0构建静态程序，SHA256 `3df31f9b555d018c17760cfbee3ab69153e46db7d5906c17eacc513dbb53b32c`。它不是Qt shared FFmpeg9.0.1；其他ELF/fixture的SHA及完整命令在私有result中。strace5.16产生采样开销，此轮不是性能对照。

## 2. 实际结果

| 事实 | 结果 |
| --- | --- |
| 单次TERM后8秒内退出 | FAIL，原现象复现 |
| 两个TERM的trace时间间隔 | 8.001243秒 |
| 两信号之间成功sendto | 1876次、1883355字节，错误0 |
| 相邻成功发送的最大间隔 | 24.125毫秒 |
| 这段单次发送的最大耗时 | 163微秒 |
| 第二TERM至exit_group(255) | 40.074毫秒 |
| 第二TERM至进程退出255 | 40.802毫秒；监督器wait返回约71.948毫秒，分母不同 |
| KILL | 未使用 |
| 服务及设备清理 | Go/media退出0/0、设备删除、receivers空、四个自有PID及进程组无残留 |

首TERM后19.774毫秒仍有成功发送，最后一次成功发送在首TERM后7.998139秒。本复现排除了“这8秒整体持续阻塞网络写”的解释；线程瞬时futex/nanosleep不能据此认定死锁。第二TERM的`Immediate exit requested`与trailer/close错误保留，退出255不是正常有限EOF。

版本标识对应的[上游ffmpeg.c](https://github.com/FFmpeg/FFmpeg/blob/1b48158a23/fftools/ffmpeg.c#L281)中，初始化后interrupt回调需要信号数大于1；该版本主循环仅在启用stdin交互时调用含信号检查的键盘分支，`-nostdin`会跳过。实际“首TERM后继续正常发送、第二TERM后中断”的行为与之相符。不过未取得此静态CLI完整构建来源，不能宣称它逐字节对应上游或不存在下游补丁，也不能将9.0.1源码当作精确版本证据。

## 3. 结论与边界

诊断本身完成、原超时可复现，目前没有据此修改Qt或media_server的生产理由；不将两个历史FAIL改为PASS。正常有限EOF可以单独验证正常收尾，但不能替代本负对照。也不删除已有在途写入、背压、源结束、worker关闭或对象析构测试，它们覆盖不同路径。

私有证据`/var/tmp/qt-publisher-term-f567e4c-g1Wbexvo/`保留版本、ELF/fixture SHA、源码、进程身份、原始syscalls、线程采样、stderr、退出码及清理结果。独立子代理只读复算了发送数量、时间和无残留结论。

| 文件 | SHA256 |
| --- | --- |
| result.private.json | `32ea3e8e47556bd88f6239a42cc0189adce898ebdd418bb96a8b1ff9ce3fb986` |
| publisher-syscalls.private.log | `04808d7ea2f94a4689b918baf0d604ea9e32636aa277526c1683aed5da7d623d` |
| publisher_term_diagnostic.py | `8b44df1d4c0478f5fb70900b0f5d45ea0b1404bdbfe406a4dc51e35c10f9c7b2` |

原始trace/日志含授权和私有控制信息，不提交Git或公开展示。此轮不涉及物理声学、真实相机按钮、完整七阶段产品GO或第三方全部实现验证。
