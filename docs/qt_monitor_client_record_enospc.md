# 录制组件的实际磁盘满补验

2026-10-17，基线919a6ac。补齐[阶段五](qt_monitor_client_stage5_record_download.md)中未执行的真实ENOSPC验证，未修改生产实现。`record_thread`、`finite_http_input`的源码与5efffbf后受测版本相同；复用独立SDK的normal/ASan `desktop_media`，仓库外probe只调用公开构造、启动、等待和snapshot接口。

## 方法与结果

固定本地H.264/PCMA素材 `pcma-long-noend.ps`，SHA `19b8925fe5031cbf35f08c10131b0edf9b00ca466b62937958fb6f8cc3d2dcdd`。每模式在独立用户/挂载namespace内创建1MiB tmpfs，预填充后仅余32768字节；先用受控write确认该文件系统确实返回ENOSPC，删除控制文件并确认空间恢复，再启动实际录制线程。不填满共享 `/dev/shm` 或宿主磁盘。

| 项目 | Normal V2 | ASan+UBSan V2 |
| --- | --- | --- |
| 进程退出 | 0 | 0 |
| 故障前已处理包 | 视频25、音频26 | 视频25、音频26 |
| 结果 | saved=false，写盘错误 | saved=false，写盘错误 |
| 清理 | 线程结束；目标和本次.part不存在 | 同左 |
| namespace收口 | 子进程回收，umount退出0 | 同左 |

Normal的strace明确记录向本次 `out.mkv.part` 的write返回 `-1 ENOSPC`。ASan不置于ptrace下，保留LSan；运行脚本显式设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`；`asan-v2-probe.stderr` 为0字节（SHA256为空文件摘要），本轮没有sanitizer诊断。probe和桌面静态库插桩，Qt/FFmpeg等依赖并非全部插桩。ASan模式的ENOSPC依据为同一容量约束和独立文件系统写入对照，不冒称采集了该模式的系统调用轨迹。

生产AVIO回调统一将短写映射为EIO，本轮界面错误为“写入录像文件失败：输入/输出错误”；这不表示内核未发生磁盘满。两模式均未保存或留下部分文件，未发现需要修改源码的回归。未重跑桌面全量CTest、Go全量或真实设备下载，不把本地文件探针等同于HTTP/摄像机端到端验证。

## 原失败与证据

首次Normal已实际观察ENOSPC且正确清理，但私有probe错误要求英文 `No space left on device`，因此原结果保持INCOMPLETE。按既有EIO合同修正私有断言，并保留独立ENOSPC对照与Normal系统调用证明；没有放宽“实际磁盘满、禁止保存、清理完成”的验收条件，也没有人为修改生产实现制造RED/GREEN。

私有证据根 `/var/tmp/qt-record-enospc-919a6ac-FJXc3jS0` 保存预声明与修正说明、源码、CMake配置、构建日志、两个模式结果、系统调用和ELF。Normal V2 ELF SHA `b56b15974717d1f237e0c020ff2a7753d8f337d488fb03d9acdfd55ea6f5b225`，ASan V2 `bbd10a9890d69293a76704238b20efca10f91d977cd9c9e89aab4b877844f08d`。首次ELF由同一源码和构建路径重建，SHA与原运行记录完全一致，重建没有执行测试；该来源单独记录，不冒称一直保留了原路径产物。

08:31中断前Normal V2已经通过；12:22恢复时只补执行尚未运行的ASan V2，没有重复Normal。最终宿主namespace无本轮挂载，相关PID消失；只清理本轮可再生对象，原失败、ELF和证据保留。
