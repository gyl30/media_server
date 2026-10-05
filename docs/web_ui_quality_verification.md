# Web UI 品质复查

## 范围与基线

BASE_HEAD：`f35aad78b756b515c5c40cb9d43e7581adfbe0c3`。本轮 UI、测试、验证记录及截图一并提交。CODE_HEAD 可用 `git log -1 --format=%H -- signaling/web/app.js` 定位，避免循环引用本提交 SHA。

生产修改仅限 `signaling/web/index.html`、`style.css`、`app.js`。保留原生 HTML/CSS/JavaScript、统一 SVG、Go embed 和现有正式 API。没有新依赖、后端 UI 状态、播放器状态字段；`whep.js`、Go 后端、C++、live/ticket/registration/SSRC 均未修改。

完整摘要与图片校验值见 [web_ui_quality.json](verification_results/web_ui_quality.json)。原始截图、日志、逐轮结果保存在 `/tmp/media_server_ui_quality-f35aad7`，包括失败记录；最终精选截图保存在本仓库的 [web_ui_quality](verification_results/web_ui_quality) 目录。

## 最终设计

- 设备采用安静的主从列表，按在线事实显示圆点与文字。20 台设备在侧栏内滚动，不再把整个页面撑高。名称自然换行，编码作为辅助信息。
- 共用播放器位于详情上方。视频是主要视觉对象，状态和播放目标放在简洁页脚。开始连接时滚动到已经显示的播放器；没有移动或复制 video、PC 或 viewer。
- 通道保留表格，降低播放按钮及状态徽章的视觉重量。窄窗口仅表格局部滚动，操作列固定可见。危险操作较安静，确认弹窗内才突出最终删除操作。
- 删除重复的空列表添加按钮、卡片边框、胶囊徽章及未使用的样式。沿用中性浅色与单一绿色 accent，间距集中在 4/8/12/16/24/32px。
- 加载、Catalog 同步、空列表、离线、请求失败和 pending 分别表达。加载期间不宣称「还没有设备」；首次请求失败也不宣称列表为空。请求反馈依据现有 controller/pending，不增加长期状态。
- 连接失败仍显示「播放失败 / 连接中断，请重新播放」；媒体结束仍显示「已结束 / 设备已离线或媒体已结束」。失败采用错误色，结束采用中性色；两者收起空画面，保留说明及通道操作。
- 关闭播放器按钮继续依据 `canStop`，停止上游按钮继续依据 current 的 liveID。没有保存旧 viewer/live 身份，也没有新增自动重连、timeout 或 retry。

## 交互与可访问性

基线真实浏览器证明，2.5 秒 polling 重建设备与通道按钮时会丢失键盘焦点。现在使用按钮已有 `data-*` 身份恢复仍存在且可用的按钮；不暂停 polling，不保留另一份选择或焦点状态。RTSP 编辑与确认弹窗的触发按钮被刷新后也能正确返回焦点。

三个 dialog 增加显式可访问名称，确认弹窗同时关联说明。保留原生 modal/focus 行为。tab 键盘切换、设备/通道/RTSP 操作焦点、滚动后的设备焦点、弹窗返回和 reduced-motion 均由实际 Chrome 检查。`display: contents` 只处理布局，CDP 无障碍树确认 active tabpanel 仍存在。

主要文字、辅助文字及正常按钮文字不低于 4.5:1；辅助文字与页面/空状态背景分别为 5.02:1 / 4.74:1，主按钮为 6.58:1。焦点描边与页面背景为 3.39:1。disabled 操作有真实禁用属性，状态同时使用文字，不单靠颜色。

普通交互过渡 160ms，弹窗 180ms；加载动画只在相应请求/连接期间出现。`prefers-reduced-motion` 关闭持续动画和滚动过渡。

## 真实截图迭代

自评分只用于暴露问题，不是客观质量认证或获奖保证。各轮完整 10 维分数见 JSON。

| 轮次 | 主要观察及后续修正 | Overall polish | 测试结论 |
| --- | --- | --- | --- |
| 0 | 卡片/徽章过重；长侧栏撑高页面；加载误报为空；轮询丢焦点；dialog 缺名称 | 5.5 | OBSERVATION，焦点 RED |
| 1 | 窄桌面设备状态错行；双加载符号；八通道偏高；桌面播放后的滚动需调整 | 8.1 | 呈现 6 项 PASS |
| 2 | 失败/结束空画面过高；删除 pending 在多行重复；需核对辅助文字对比度 | 8.5 | 呈现 11 项 PASS |
| 3 | 辅助文字 4.4:1 尚不足；需覆盖 RTSP 弹窗轮询；窄窗操作列部分不可见 | 8.6 | 呈现 11 项 PASS；对比度继续修正 |
| 4 | dialog close 事件尚未完成时断言焦点；收起画面变窄；fixture 缺 username | 8.7 | 呈现 FAIL，原始记录保留 |
| 5 | 修正事件边界等待、完整 fixture、画面宽度与结束色彩；复查无明显短板 | 8.9 | 呈现 13 项 PASS |
| 最终 | 增加四种宽度操作可见、完整 Tab 顺序和后台 inert 检查；核对最终静态资源 SHA256 | 8.9 | 呈现 19 项及真实媒体 25 项 PASS |

第 4 轮未作为成功证据。独立浏览器诊断确认焦点恢复的 close 回调会完成；测试改为等待实际焦点到达正确按钮，最多 2 秒，不以 sleep 后忽略错误。补齐的 username 是正式 API 的 fixture 字段，没有为测试修改生产输入处理。

额外 Tab 检查的首次断言也保留为 FAIL：它误要求原生 dialog 阻止焦点进入浏览器工具栏。独立诊断确认该时刻 `document.hasFocus()` 为 false，页面后台仍为 inert。最终测试记录控件顺序，并要求每次页面内焦点都在弹窗中；没有实现新的自定义焦点 trap。

最终 10 维自评：Layout 8.9、Typography 8.8、Spacing 8.9、Color 9、Hierarchy 9、Interaction 9、Consistency 9、States 9、Accessibility 9、Overall polish 8.9。最终实际截图复查未发现明显值得继续修改的品质问题。

## 浏览器与功能验证

Chrome `153.0.8010.52`。密度/呈现使用真实 Go embed 页面与 Chrome，仅替换管理 API 响应；这部分不证明 SIP 或媒体生命周期。覆盖：

- 1440×900、1920×1080、1000×850、600×900。
- 0/1/5/20 台设备；0/1/8 通道；长设备/通道名称；5 个含长地址及不同取流事实的 RTSP 源。
- 空、加载、离线、Catalog 未完成、Catalog 后为空、网络失败、删除 pending/失败、键盘焦点、弹窗和减少动态效果。
- 页面没有水平溢出，四种宽度的通道播放按钮完全位于可见横向范围。

最终呈现检查 19 项 PASS，31 张原始截图，无 pageerror。真实完整 Browser E2E 25 项 PASS，82.98 秒，无 pageerror，使用实际 SIP simulator、RTP/PS、WHEP、H264 解码及 RTSP H264/Opus 双轨。未以 fake PeerConnection 替代网络或解码。

真实验证保留三 viewer 共享一路上游、失败/正常关闭后 live 保留、重播复用、停止整路 live 后所有 viewer 结束、Expires:0/重新注册、新 generation、票据实际过期后一次 fresh ticket、迟到 201 清理、删除及 autoplay 行为。新增停止请求 502 后 viewer/live 仍可用的检查，并保存连接中、关闭中、失败与结束截图。HTTP 异常与 failed 事件边界使用故障注入；正常媒体仍真实执行。

Transport failure 后 live 保留，RTP 计数 **537→705**，重播 INVITE **1→1**，使用新 ticket。按钮在 idle/preparing/negotiating/streaming/failed/ended/stopping 事件中均与实际 current ownership 相符。两个最终套件记录的静态资源 SHA256 与当前工作区逐项一致。

## 其他检查与边界

`go test -count=1 ./...`、`go vet ./...`、`go test -race -count=1 ./...`、`cmake --build build -j12`、CTest **14/14** 全部 PASS。CTest 在媒体 E2E 进程退出后执行。C++ 零修改，没有重复 sanitizer 验证。`git diff --check`、JavaScript 语法检查、Python 编译检查通过。

生产代码物理净行数 -717，其中 CSS 971→196 行包含选择器收敛和紧凑排版，不能将全部行差当作算法删除收益。实际删除了重复展示与未使用样式；新增 JavaScript 主要是焦点恢复、请求反馈和自然中文。没有修改媒体算法、线程、队列、网络或业务 ownership。

这是 Chrome 下的桌面及窄窗验证，不声称所有浏览器认证，也不声称一定获得设计奖项。后续应由真实使用问题推动修改，不继续增加状态、框架或装饰效果。

## 异步交互 correctness 复查

本次 BASE_HEAD：`23e232847775d6d57d83626070b7cefbb791a6aa`。生产修改仅限 `app.js`、`whep.js`，新增 59 行、删除 24 行，净 +35。Go 后端、C++、CSS、HTML、API、数据库和 live/ticket 模型零修改。

- resource action 用函数局部 trigger 跨越请求与 DOM 重建，完成后恢复同语义按钮；消失的 source/device 回到添加入口，停止操作回到该资源仍可操作的按钮。用户已转移到其他有效控件时不抢回焦点。polling 移除聚焦资源时也回到列表入口，用现有 pending Set 区分请求中的临时禁用。返回列表恢复设备项，hash/tab 切换恢复对应 tab，显式关闭 viewer 恢复原播放按钮或当前列表入口，autoplay 恢复按钮隐藏前移交焦点。
- device/source dialog 用一次性 close listener 恢复 trigger，删除 `sourceDialogTrigger`。source 提交期间 save/close/cancel 均禁用，Esc 被阻止，重复提交和重新打开沿用 `saveSource.disabled` 防护；请求结束恢复控件。confirm 仍只确认操作，不拥有后台请求。
- global status 全部自动消失，默认 5 秒；持续失败刷新同一个 timer，quiet polling 恢复后自然消退。表单、player 与通道错误仍属于各自 inline 组件。
- viewer ownership 与 panel visibility 分开：failed/ended 时 current 已清空，但允许点击关闭收起面板，不再发送 DELETE；stopping 时关闭按钮禁用，idle 时面板隐藏。删除仅有一个调用点的 `canStop`。这替代前文历史验证中的「closeDisabled 等于没有 current」假设；停止上游仍只从 current.liveID 推导。
- `closeCurrent()` 返回 null 或 DELETE 错误；无论远端结果如何，本地 PC/video/current 均先清理。显式关闭失败复用 `whep_delete_500` 文案「播放器已关闭，服务器资源尚待清理」，内部 replay/tab/device cleanup 忽略返回值。没有保存 cleanup error 成员。
- ICE gathering 入口先检查 already-aborted，与已有 delay 等待一致。保留 `session.streaming`、pending Set 和 generation fencing；不新增状态字段、timeout、retry 或依赖。WHEP POST 仍不绑定取消信号，迟到 201 的实际 Location 继续用于删除资源。

测试继续使用 `bench/gb_web_ui_verify.py`，通过正式 UI/API、HTTP response hold 与真实原生 PC 的事件边界验证。ICE 测试先调用原生 setLocalDescription，再暂停其返回并固定该测试 peer 的 gathering 观察值；关闭后释放 await，要求 start 在 3 秒内结束且不发 POST。未替换网络/解码实现，生产没有 test hook。

原始 RED 与迭代失败均保留在 `/tmp/media_server_web_async-23e2328`。原缺陷覆盖 dialog pending、删除焦点、永久 status、failed dismiss、DELETE feedback、already-aborted ICE、hash/autoplay 焦点和 polling 删除焦点。本轮实现曾将 channel fallback 匹配到 tr，真实 GB stop 测试发现后限定为 button；没有隐藏失败。HTTP hold 测试改为等待提交后的真实刷新结束再移除 route，避免后继 GET 仍被测试拦截。

最终 Chrome `153.0.8010.52` 完整真实媒体 E2E **34 项 PASS**，111.24 秒，33 次具体语义焦点断言，无 pageerror。结果为 `full-accepted/result.json`，摘要追加在既有 JSON 的 `async_interaction_followup`；静态资源、后端文件及测试文件哈希已核对。覆盖 GB 三 viewer、INVITE 复用、live stop/Expires:0、票据真实过期、迟到响应与 RTSP H264/Opus。transport failure 后 live 继续，RTP 615→771，重播 INVITE 1→1。

最终 `go test -count=1 ./...`、`go vet ./...`、`go test -race -count=1 ./...`、normal build 与 CTest **14/14 PASS**。媒体 E2E 全部进程退出后才执行 CTest。C++ 零修改，按本 Goal 要求不重复 ASan/UBSan。JavaScript 语法、Python 编译、diff check 通过；没有永久 global danger、旧 ownership 测试假设或新增冗余状态字段。
