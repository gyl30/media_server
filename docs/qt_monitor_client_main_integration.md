# Qt 客户端合入前回归与边界

2026-10-18，按用户要求停止功能扩展，将已有 Linux 客户端稳定后合入 main；未实现功能另列于[后续清单](qt_monitor_client_followups.md)。主线起点为 `ba803c2`，本次受测实现为 `b14ab49`；其后的 `f6403bc` 仅新增/修正文档，没有改变生产或测试源码。本记录同样只改文档，不把历史实机测试冒称为本轮重新执行。

## 本轮实际执行

DEV 私有证据目录：`/var/tmp/qt-main-integration-b14ab49-7gsE2Duh/`。包含执行脚本、命令输出、各步起止时间与退出码、四条 lane 的完成收据、CTest LastTest 日志和受测源码 SHA 清单。

| 检查 | 结果 | 实际范围 |
| --- | --- | --- |
| 桌面 Normal 构建及完整 CTest | PASS | 10/10 组，484 项 QTest，0 失败、0 跳过；995.65 秒 |
| 桌面 ASan/UBSan 构建及完整 CTest | PASS | 10/10 组，484 项 QTest，0 失败、0 跳过；1000.51 秒；启用 LeakSanitizer |
| 后端 RelWithDebInfo 构建及完整 CTest | PASS | 71/71，521.20 秒 |
| 后端 ASan/UBSan 定向构建与 CTest | PASS | gb_sender_lifecycle、gb_audio_sender、gb_audio_upload、gb_playback_lifecycle、gb_playback_http，5/5，90.33 秒 |
| Go test | PASS | `go test -count=1 ./...`，主包 142.416 秒，simulator 0.003 秒 |
| Go race | PASS | `go test -race -count=1 ./...`，主包 143.324 秒，simulator 1.011 秒 |
| Go vet | PASS | `go vet ./...`，退出 0 |
| Web 对讲 worklet | PASS | `node tests/web_console/talk_worklet.mjs`，包含重采样、PCM 对照及有界 credit |
| CDP 归属负对照 | PASS | `node tests/web_console/cdp_ownership_test.mjs`，已有 CDP 端点未被访问；预期子进程失败不等于工具失败 |
| 分支差异与身份 | PASS | 合入前 `git diff --check main..HEAD`；生产/测试源码 SHA 清单校验；`third/**` 和 `designs/**` 无分支差异 |

Normal 使用 RelWithDebInfo、`-O2 -g -DNDEBUG` 和既有 `-Werror`；桌面 sanitizer 使用独立构建、`-O1 -g -DNDEBUG` 与 `-fsanitize=address,undefined`。后端 sanitizer 为独立 Debug 构建，使用既有 Boost 1.92 ASan 前缀，未修改正常 Boost 或第三方子模块。运行启用 `detect_leaks=1`、错误即停，原始日志未见 ASan、UBSan 或 LeakSanitizer 错误。第三方未全部插桩，不将此结果扩张为所有依赖均已完成内存安全证明。

## 保留的实机证据和限制

- [安装版真实录像控制与主动关闭](qt_monitor_client_stage7_installed_controls.md)保留指定大华录像的实际解码、2→1 倍率、seek、SIP 及退出证据；本轮没有再次修改相机或重复执行该实机测试。
- [第二机 16 独立源](qt_monitor_client_stage7_second_multisource.md)仍绑定原产物、媒体与硬件；不是本轮新容量基线，也不代表 16 台相机验收。
- 历史源替换轮次的 PARTIAL/FAIL_CLEANUP、发布者信号退出和工具误选窗口的原始记录不改写为 PASS。
- 物理双向对讲声学仍未闭合；数字 PCM 和协议组合不等于真实麦克风/设备扬声器验收。
- 项目许可证及完整依赖源码交付仍有公开分发边界。用户授权代码合入 main，不等于公开二进制发行无条件 GO。

本轮不删有意义的测试、不放宽健康标准、不增加生产状态或协议功能。原型、内部运行包、ELF、原始失败和实机证据继续保留；编译中间物的实际清理由私有逐文件清单单独记录，不能由本报告推定已删除。
