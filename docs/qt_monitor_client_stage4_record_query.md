# Qt 阶段四：录像目录查询子集

基线 `a0ea324973291f6becd110c6fec2466d33923aaf`，只在 `feature/qt-monitor-client` 开发。本子集检索 GB28181 设备自报的录像目录，不建立历史回放、下载或实时会话，不表示整个阶段四完成。

## 协议与接口

复用 signaling 原有 SIP MESSAGE、注册设备路由、SN、`pendingQueries` 与 `live.mu`。新增：

```http
POST /api/devices/{device_id}/channels/{channel_id}/records/query
Content-Type: application/json

{"start_time":"2026-10-15T00:00:00","end_time":"2026-10-15T01:00:00","type":"all"}
```

父 ID 和通道 ID 均为20位数字，通道须在当前在线父设备的 Catalog 中。时间严格为 `YYYY-MM-DDTHH:MM:SS`，真实日期且开始早于结束，不接受小数、时区后缀或规范化后的不同字符串。它是设备当地墙上时间，不把设备时区猜成 UTC，也不做时区换算。类型显式为 `all/time/alarm/manual`。非法输入在发 SIP 前返回400；不存在、离线、通道不属于设备和平台停机沿用原有查询错误语义。

查询 XML 的 CmdType 为 `RecordInfo`，携带本次 SN、通道 DeviceID、StartTime、EndTime、Type。原有 DeviceInfo、DeviceStatus 和 PresetQuery 仍沿用既有5秒总期限及原 XML/JSON；新查询总期限15秒，Qt请求期限20秒，二者是本次应用约束，尚未通过真实录像设备校准，不是标准规定的期限。

依据 [GB/T28181—2016](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf) 附录 A.2.1、A.2.4、A.2.6 及 N：RecordInfo 响应不要求 Result，空结果可只有 SumNum=0 而没有 RecordList。顶层及 Item 的 Name 字段必须存在，但允许空字符串；Item DeviceID 必须与目标通道一致。可选字段原字符串保留，Secrecy 缺省为字符串 `0`。不把缺省可选时间或路径当作非法记录，也不凭目录项断言文件存在。

## 分批、边界与错误

相同 SN 的多批响应仅在实际收到的 Item 总数达到设备声明 SumNum 后返回HTTP200。每批 Num 必须与实际 Item 数相同；首批固定总量，后续改变总量或累计超量返回 SIP400，不污染已收到的有效批次。正总量的 Num=0 批是无进展响应，仍等待后续批次或总期限，不误报成功。响应还须匹配在线注册父 From、实际来源、SN、CmdType 和目标通道，注销、来源改变、取消和超时保留既有身份检查与清理。

新状态只有 pending 查询中的 `expected` 和 `records`：前者是设备声明总量，后者保留已接收批次。nil 区分尚无有效首批，显式空 slice 表示收到合法零条；不增加完成/关闭标志、第二个 registry、DB 字段或后台任务。多批累积在原互斥锁内，最终返回独立 slice，不让后续响应修改 HTTP 序列化中的目录。

每次查询最多1000条，这是有界应用结果限制，不是国标的每批最大值。合法且匹配的 SumNum 超限返回 SIP200，HTTP422 `record_result_too_large`，提示缩小时间范围；通过本次响应的非序列化失败值传回原等待流程，不增加 sticky error、回滚或重试。单 XML 继续受原64KiB限制；该限制、UDP传输大小及对端分批能力仍可能影响检索，不能宣称支持任意规模目录。

不按文件路径、时间或内容去重，合法完全相同的 Item 仍可出现多次，按实际收包顺序保留，不保证 UDP 的设备发送顺序。锁定的 sipgo v1.6.0 会在 SIP 事务层处理相同 Via branch/sent-by/CSeq方法的同事务重传，不再次调用业务 handler；换新 branch 的重复业务 MESSAGE 与合法相同记录无法区分，可能计入 SumNum。因此界面只显示“设备声明 N 条，已收到 N 条”，不是文件唯一性或设备库存完整性证明。历史回放必须使用独立会话，不得据此复用实时 live identity。

## Qt 所有权及交互

仅国标通道可打开独立非模态“录像查询”窗口；开窗不自动发送请求。窗口固定父设备与通道，树选择变化不改当前目标。重新打开关闭旧窗口；刷新设备、切平台和主窗口关闭会终止旧查询。显示10列：通道、名称、开始、结束、类型、文件路径、地址、保密、触发者、大小，均为纯文本，不打开设备提供的路径或 URL。

`record_query_dialog` 持有实际请求 context 和 QPointer reply；取消、done、析构先销毁请求 context，再 abort reply，避免同步 finished 回调写入关闭中的控件。主窗口析构体在 client 成员销毁前关闭对话框；listing 销毁关闭对应查询窗口。只有真实窗口指针，没有 prepared/started/busy/closed 业务状态。按钮使能由请求流程直接设置。

录像查询不使用 PTZ 的 control_context，不清理 PTZ owner；PTZ 或设备查询完成也不改录像查询的忙碌状态。取消只结束本次目录请求，不停止实时 viewer、共享上游或其他查询，不自动重试。成功HTTP还须严格核对父/通道、RecordInfo、正整数 SN、0–1000 总数、完整数组计数及各字段类型；身份或格式错误不展示半份目录。

## 验证记录

| 验证 | 实际结果 |
| --- | --- |
| Go查询定向 | PASS，25项顶层，36.161秒 |
| Go全量test / race / vet | PASS，各109项顶层；signaling69.099 / 75.731秒，simulator0.002 / 1.008秒，vet退出0 |
| 桌面normal完整构建与CTest | PASS，5/5，109.01秒，311项QTest，无失败或跳过 |
| 桌面ASan+UBSan完整构建与CTest | PASS，5/5，111.14秒，311项QTest，无失败或跳过，无sanitizer报告 |
| 实际Qt→HTTP→UDP组合 | PASS，私有run02应用1.564秒、Go组合1.762秒 |
| 真实录像设备目录与历史回放 | BLOCKED / NOT_RUN，未继承其他阶段厂商历史PASS |

新增12项Go RecordQuery顶层用例覆盖：合法零条两种形式、无进展批、早于查询SIP200的多批、坏批后继续、总数变化、超量、1000条（200批×5条）、GBK/转义及原始可选字段、同事务原样数据报重传只进入业务一次、新事务相同Item保留、错误身份/来源/SN/命令/目标、注销、HTTP取消、实际15秒超时、并发通道及旧查询、旧XML/JSON逐字节契约。没有以SIP200代替目录完整。

新Qt查询suite114项，窗口完整150项，另有12项平台请求、11项解码和24项播放生命周期，合计311。覆盖纯文本/合法重复/1000条/空Name/可选字段、严格身份及字段类型、4种type、墙上时间、422及手动重试、真实20秒期限、挂起请求取消、close/reject/直接析构/父析构、旧200/502隔离、重新开窗/刷新/平台更换、固定目标及PTZ/实时请求独立性。实际解码用例提供CLI，未以跳过替代通过；该独立性用例本身不冒充媒体解码。

独立Qt增量验证normal与ASan+UBSan也通过（查询114项、窗口150项）；最终从新的构建目录重新执行完整桌面5项。普通构建使用`-O2 -g -DNDEBUG -Werror`，sanitizer使用`-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer`，实际链接libasan/libubsan，未关闭LeakSanitizer。Qt、SDL及共享FFmpeg没有全部插桩，结果不扩大成整个服务器或全部第三方内存安全证明。

实际run02从独立私有Xvfb启动最终gb_monitor ELF，以真实鼠标打开查询窗口（零SIP）再发查询。peer收到唯一RecordInfo/SN1，分两批各一条；首批不产生最终结果，界面显示两条目录及“设备声明2条、已收到2条”的边界。根代理实际查看最终中文截图。最终pending/live/PTZ owner/INVITE均0，父注册对象未变；应用与Xvfb均退出0、reap，PID1004361/1004358核实消失。这里只预置注册/Catalog fixture，未声称真实REGISTER或摄像机。

原始失败均保留：`route-red-02.log`是旧接口404→新接口409的有效RED；`route-red.log`误写`-p1`不是业务RED；`directed-02.log`测试调用不存在的方法是编译错误；`directed-03.log`的每包250条fixture触及sipgo发送端MTU限制，改为200批×5条后同1000条边界通过，未改系统/第三方或健康断言。Qt首轮4行测试错误地向QDateTimeEdit传入带偏移QDateTime、误判其显示时刻，已用控件date/time输入明确墙上时间修正fixture，未改生产。GUI run01猜测Tab焦点未触发查询，应用失败清理退出-15，原记录保留；run02仅根据实际截图校准私有脚本的鼠标点击，未改生产。

Go原始日志保留sipgo关闭引用计数警告及显式取消后的UnhandledResponse，不隐藏、也不宣称已证明无害；未发现测试失败或race报告，未修改第三方。该警告的依赖内部归因仍是验证边界。

- Go RED/GREEN及真实UDP事务：DEV `/var/tmp/media-server-qt-record-query-zk3R88Gs/`。
- Qt独立HTTP与生命周期测试：DEV `/var/tmp/media-server-qt-record-ui-tests-WtTP1nRN/`。
- 本子集最终构建与日志：DEV `/var/tmp/media-server-qt-stage4-record-main-6S4nzeFB/`。
- Claude设计讨论：DEV `/tmp/qt-client-plan-claude-JuZHjy/round-26.json` 至 `round-28.json`，第28轮确认不做无协议依据的内容去重。

Claude第29轮完整生产代码复审无P0/P1，要求的分批/重传/旧契约、实际Qt组合及normal/sanitizer全量回归已兑现。最终GUI ELF SHA256：`aa978fcbda16c852f41387863507b606e0b3c8b78730c8f84349858a653bf73a`；`gui-run-02/result.json` SHA256：`e6f999bdc33e6ce5503c516415551cbb1efbfd9c3f6dcc92eb7a6b1bcb743972`；完整GUI日志SHA256：`3a9eb7863bab4c8743c9c91fe254cdb87fb41869190b1dc04f5ae263014f22b9`。全部构建ELF校验见最终目录`build.sha256`；原始证据可能包含授权，不提交原始日志或截图。

额外发起的第30轮最终文档复核被Claude会话额度限制阻断，原始CLI退出1及返回JSON保留，不能记为审查PASS。当前子集以第29轮实际生产审查和随后逐项兑现的测试门槛为依据；下一子集设计仍须等待Claude讨论，不绕过该流程。

按阶段清理要求，核对独立增量测试目录的真实路径、CMake来源及无运行测试后，只删除其中normal/asan的CMakeFiles对象（31276/53360 KiB）。Ninja配置、编译命令及ConfigureLog已单独归档，测试ELF、Testing、源与全部原始失败保留。最终完整normal/asan构建保留供下一阶段复用；没有清理历史数据、他人目录或系统依赖。

真实录像摄像机的目录、多批顺序、设备时钟及后续历史回放尚未验证。受控 SIP peer 只能证明本平台实际 HTTP/SIP 路径，不冒充真实设备库存、文件可读性或历史媒体解码。公共发行许可边界不因本次查询功能而改变。
