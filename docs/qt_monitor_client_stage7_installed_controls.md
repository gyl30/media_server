# 安装版录像控制与主动关闭接手验证

2026-10-18 接续 `7691504`，仅修改仓库外监督和观察脚本，生产代码、测试与依赖不变。最终补齐同一安装版的真实录像查询、2×→1×、seek及播放中关窗的限定证据；原始失败全部保留，整产品仍未GO。

## 1. 产物与方法

DEV使用既有clean `d06f0c0` 内部包，从 `/` 启动 `run.sh → gb_monitor`；安装ELF SHA256为 `8dcf4a71e613f8dcee9aae3dbc1875c0db0b7026b283a647bf2f6ba3461273d8`，manifest为 `250917ea868a1238fc49cee7f4ffc3ba698c21dbb4f8b25083e2ab91bd2ff505`。每轮逐项核对236条包文件SHA和实际Qt/FFmpeg/SDL/XCB映射。它不是当前HEAD新构建；`d06f0c0..7691504` 桌面生产源码无差异，CMake差异仅在测试分支。

TEST媒体ELF为 `45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4`，Go signaling为 `57f38db00d45b3f34b8b97bf01704d7ac2ca23a41d74a0af613db7f1efff2caa`。独立平台使用SIP15060、TCP38470–38473；DEV私有代理 `127.0.0.1:27473` 只原样转发 `/api/`，记录请求、上游完成和下游写出，不伪造控制响应。

私有认证Xvfb为1280×800，输入只经外部XTest鼠标/键盘；日期通过自有xclip及键盘日字段修正，并核图、核对真实查询body。没有Qt内部setter或生产hook。读取大华原63秒录像 `2026-10-17 14:44:25–14:45:28 UTC+8`，查询范围14:44–14:46、偏移480分钟，返回一条记录。相机配置和库存未修改。

## 2. 失败与中断保留

| 轮次 | 原结果与原因 | 保留边界 |
| --- | --- | --- |
| 交接run01 | PARTIAL、FAIL_CLEANUP；真实seek5和Scale1，没有Scale2请求、没有DELETE；自然121/BYE后关窗15秒未退，最终Qt TERM−15 | 原监督器未记录选中的XID，不能事后认定为产品缺陷或已证明的popup误选 |
| run02首attempt | FAIL_CLEANUP；提交了含下划线的快照名，监督器拒绝后异常收尾，Qt TERM−15 | 尚未请求录像播放；不是实机关闭验收 |
| attempt02 | TEST端口预检EADDRINUSE，服务启动前失败；后续逐端口探测见38472 TIME-WAIT。6次代理GET连接被拒绝，故FAIL_CLEANUP | 原异常未记录具体端口；空页面主窗退出0不等于录像播放中关闭通过，未改系统网络参数 |
| attempt03 | 会话中断后TEST服务已STOPPED，DEV监督器消失，Qt/Pulse/Xvfb/parec成为孤儿 | 尚无录像播放；按保存的PID/starttime回收，主窗WM_DELETE约40ms后消失，但原退出码无法取得，不补造PASS或退出0 |
| attempt04 | 真实控制与Qt主动关闭已收集；代理把DELETE响应写向已退出Qt时的BrokenPipe一概判为FAIL_CLEANUP | 原失败不改写。紧邻seek的2×快照窗含跳变/追赶，不能用约59fps当稳态2× |
| attempt05 | 固定窗口的录像控制及主动关闭证据确认；原监督结果为 `OBSERVATIONS_COLLECTED_PENDING_REVIEW`，离线收据为限定确认 | 数字音频、虚拟屏幕与指定库存范围；不扩张为物理声画、全部设备或整产品GO |

上述DEV目录依次为 `/var/tmp/qt-installed-camera-7691504-AljWGViN/`、`/var/tmp/qt-installed-camera-run02-7691504-YnDMdDRn/` 和 `/var/tmp/qt-installed-camera-run02-attempt02-7691504/` 至 `attempt05-7691504/`。TEST首run02目录为 `/var/tmp/qt-installed-camera-run02-7691504-yHSUA4WE/`，后续attempt与DEV同名。

attempt02/03的GUI脚本相对交接assets仅更改证据根目录。中断后新增脱离工具会话的自有监督进程与命令FIFO，保留原GUI600秒、服务900秒及关窗15秒期限。attempt04还保留了首次误查10月18日返回0条的请求、截图，以及随后纠正到17日取得一条录像的事实。

## 3. attempt05真实控制与媒体观察

先等待该运行中的平台收到相机重新注册，再刷新通道；没有因为暂时离线而反复重启。播放前保存固定计划：首次实际画面后观察基线8.2秒；Scale2/Scale1分别在真实204之后等待2秒，再观察8.2秒；最后seek5、观察2秒并在播放中关闭。表中为实际快照记录间隔，包含少量采样开销。

| 窗口 | 间隔 | 解码计数 | 显示计数 | 解码/显示fps |
| --- | ---: | --- | --- | ---: |
| 基线 | 8.356124秒 | 26→233 | 13→222 | 24.772 / 25.012 |
| 实际请求2×后的固定窗 | 8.374629秒 | 465→881 | 454→875 | 49.674 / 50.271 |
| 恢复1×后的固定窗 | 8.375557秒 | 975→1185 | 973→1182 | 25.073 / 24.954 |

这些是实际解码和交给Qt画面的计数。2×判断来自约50fps及录像画面时间的进展，不来自下拉框或204；不保证物理显示器扫描到每帧。seek前后屏幕录像时间由14:45:13回到14:44:32，关闭前仍显示录像回放、解码1265/显示1262帧。seek受理后的首张截图被结果文案占用，没有帧计数，不补造该值或精确落点。

每个窗口两端均只有一个归属Qt PID4012301的Pulse sink-input，48kHz、双声道s16le，未静音且未cork。monitor独立采样为8kHz单声道s16le：

| 窗口 | PCM样本/时长 | RMS | 非零样本 |
| --- | --- | ---: | ---: |
| 基线 | 66858 / 8.357250秒 | 19.559 | 65818 |
| 2× | 66987 / 8.373375秒 | 22.419 | 65305 |
| 恢复1× | 67029 / 8.378625秒 | 14.577 | 65517 |

本轮只确认数字音频持续输出与归属，未单独复算PS音频PTS速率、逐样本完整性或宽带音质。较强的双轨输入/呈现与sanitizer证据仍分别绑定[真实Qt倍率报告](qt_monitor_client_dahua_qt_scale.md)和[受控呈现回归](qt_monitor_client_stage4_scale_presentation.md)，不能互换版本或范围。

## 4. SIP与主动关闭

独立从原pcap过滤SIP15060/HTTP38473，再以tshark双遍重组核对：一条playback POST201，同一owner的Scale2、Scale1、seek5均POST204，唯一DELETE204。一个Playback INVITE/ACK dialog内，INFO CSeq199/200/201依次为Scale2、Scale1、Range npt=5-，均有200；Scale1及seek各有同事务重传请求，原始wire记录保留。最后BYE CSeq202获200，未在本抓包中找到MediaStatus/121自然结束事件。

严格按唯一中文标题、NORMAL、IsViewable、非override选择主窗。主窗XID4194310，旧算法最后候选4194330实际为隐藏override popup；这再次证明工具存在同类误选条件，但不倒推run01当时的XID。

同一Qt/代理单调时基的关闭顺序为：

| 事件 | 相对监督起点 |
| --- | ---: |
| WM_DELETE提交 | 149.499511秒 |
| 代理完整收到DELETE | 149.502807秒 |
| Qt退出0 | 149.613265秒 |
| 代理读完上游204 | 149.998971秒 |

Qt在113.754ms内退出0。代理随后回写已关闭客户端时仍产生原始BrokenPipe，Qt没有收到该204。attempt05仅在**处理下游回写异常时**已读取同owner DELETE的204、已发关窗且Qt退出0、此前该owner控制真实204的条件下，将其记录为预期响应取消；其他请求、上游错误、非204、错误owner或非正常Qt退出仍判失败。没有要求或声称DELETE在WM_DELETE之前完成。

独立复核曾将此条件解释为“关窗前已完成204”；主代理依据实际条件、上表同一时基和原始收据驳回该解释。原计划、脚本及错误文本均保留，未调整15秒期限或把未完成的上游删除算作成功。

## 5. 清理、证据与限制

Qt/parec/Xvfb/Pulse均退出0，自有PID/进程组消失，xauth删除，代理线程/handler结束、27473可重绑。确认鉴权 `/receivers` 为空后才停止TEST服务；Go/media/capture均退出0，PID/进程组消失，UDP15060及TCP38470–38473逐项不设reuse重新bind成功。

源DB SHA保持 `6fe84a5d615684416ebdee851d17a90df30eeb7aafedd6cf347db7e6fafd842d`，源及本轮DB的Broadcast高水位均为15。每轮新DB由只读源备份，未覆盖已使用DB、回退SourceID或删除相机数据。原始JSON、pcap、PCM、截图、脚本和ELF身份只留私有目录。

attempt05 DEV `/var/tmp/qt-installed-camera-run02-attempt05-7691504/`，TEST同名；DEV `test-evidence/`保存TEST冻结副本。独立只读核验和主代理复算分别核对协议、截图、PCM及取消条件；最终值按原始整数帧差和elapsed重新计算。

| 文件 | SHA256 |
| --- | --- |
| `result.private.json` | `fdf65bd691ddedffa4ecbff5a526480f24264e559aa7470ae9321f26c72a3898` |
| `api-receipts.private.json` | `6858512e55d4abc00dd27638c78014a8a1a1c9bcef1d80abe713eaf6c91e9717` |
| 原 `capture.private.pcap` | `ee554bbf718327e38ad99fb6c4967e579763769817b67cc1eb8a4e0a6eeea355` |
| `evidence-receipt.private.json` | `66affd886f62b2bed731321333f3abcb39f85d67228fc01abd4a7735d12b6bf1` |
| `SHA256SUMS`（152个文件，实际校验退出0） | `8899d25fbe2b2daa8d61c30ad8cbfdfc348a8b7c23d9544d89a5f9c30a0dcbc4` |

本轮不是ASan安装包，没有重跑或冒称新的484项全套。生产源码及测试未改，不增加关闭状态或等待HTTP响应的产品逻辑。[授权前挂起关闭](qt_monitor_client_stage7_pending_close.md)已有独立PASS；本报告补充已建立录像媒体的主动关闭，两者不互相替代。[源替换两轮](qt_monitor_client_stage7_source_cycle.md)的A窗口PARTIAL、发布者TERM清理FAIL仍保留。

## 6. 七阶段与审计收口

下表汇总当前证据，不把历史报告中的“尚未”自动升级为新待办，也不把后来的通过倒写进旧轮次。

| 阶段 | 当前可核对交付 | 剩余边界 |
| --- | --- | --- |
| 1 基础观看 | [基础播放、取消、音轨与数字同步](qt_monitor_client_stage1.md)及后续真实大华播放 | 物理屏幕/扬声器不由数字回环代替 |
| 2 多画面 | [第二机16独立源](qt_monitor_client_stage7_second_multisource.md)，共同30秒、1950断言及资源回收 | 指定fixture/CPU解码条件，不是16台相机或容量上限 |
| 3 GB控制 | [真实左右点动、STOP、预置位保存/调用/删除与原位恢复](qt_monitor_client_dahua_controls.md)及Qt→HTTP→SIP回归 | 300ms START迟到确认兼容限制仍在；变倍PARTIAL，其他物理动作未逐项验收 |
| 4 设备录像 | [真实回放/暂停](qt_monitor_client_history_deadline.md)、[同会话seek](qt_monitor_client_dahua_qt_seek.md)、[Scale双轨](qt_monitor_client_dahua_qt_scale.md)及本轮安装版操作 | 指定设备/库存；不承诺精确定位、所有设备全部倍率或物理音质 |
| 5 下载/录制 | [六组下载及八组实际view本地录制](qt_monitor_client_stage5_record_download.md)、真实大华下载、[实际ENOSPC](qt_monitor_client_record_enospc.md) | 不新增逐项安装版鼠标门禁；其他NVR、超大文件和长时录制未外推 |
| 6 报警/对讲 | [Qt→生产HTTP→真实UDP报警](qt_monitor_client_stage6_alarms.md)、[Qt](qt_monitor_client_stage6_qt_talk.md)/[Web](qt_monitor_client_stage6_web_talk.md)上传及[真实大华公共对讲数字链](qt_monitor_client_dahua_public_talk.md) | 物理报警触发未验，可选订阅/持久化未实现；物理麦克风、设备扬声器及双向声学仍INCONCLUSIVE/未验 |
| 7 综合交付 | 第二机运行包、16路、源替换/重启观察、授权前与媒体中关闭、历史Normal/ASan证据 | 原始FAIL/PARTIAL保留；[项目许可及完整依赖源码交付](qt_monitor_client_package_provenance.md)仍BLOCKED，未公开发布 |
| 遗留深度审计 | [调用链、负对照、全套与保留理由](qt_monitor_client_legacy_test_audit.md)已完成；仅删三个无消费者返回副本 | 本接手未发现新的生产删除候选，未删除测试；历史482项全套和新增19项定向保留各自版本 |

交接内可独立完成的安装版验证与报告已收口。物理声学、上述设备兼容/未验范围及公开分发材料需按原边界继续处理；不据本轮宣布整平台无条件GO，不自动合并main。
