# Qt 阶段三：通道预置位只读查询

## 范围

基线为 `426626e7cbdc11b497b9a6208a8d25e347e76b72`，在 `feature/qt-monitor-client` 增量实现 PresetQuery。没有实现 PTZ 或预置位设置、调用、删除，不触发物理动作；整个阶段三尚未完成。播放器、媒体发送、一次性播放授权及数据库不变。

## 接口与设备协议

```http
POST /api/devices/{device_id}/channels/{channel_id}/presets/query
```

不需要请求体，Qt 发送 `{}`。父设备及通道编号均沿用 20 位数字校验；通道必须存在于该父设备当前 Catalog，父设备必须在线且未进入停止。结果如下：

```json
{
  "device_id": "34020000001320000001",
  "channel_id": "34020000001310000001",
  "command": "PresetQuery",
  "sn": 1,
  "presets": [{"preset_id": "camera-A", "preset_name": "入口"}]
}
```

`presets: []` 是合法空列表。该查询响应没有 `Result` 字段，不借用 DeviceInfo/Status 的 OK/ERROR 判断。依据 [GB/T 28181—2016](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf) 附录 A.2.6 的预置位查询响应，`PresetList` 和其 `Num` 必需，数量必须等于 Item 数；编号为非空、无重复的字符串，名称可为空。查询编号不套用预置位控制指令的 1–255 数值范围，不做整数转换或截断。未引入 `SumNum` 或 Catalog 分包聚合逻辑。

HTTP 参数错误 400，父设备或通道不存在 404，离线/停止中 409，SIP 拒绝 502，5 秒内未收到合法匹配响应 504。Qt 仍使用已有 10 秒 HTTP 期限。SIP 200 不是业务查询结果；先到 XML、后到 SIP 200 可以成功，随后 SIP 拒绝仍失败。非法 XML 回 SIP 400，不交付 pending；有期限内后续合法 XML 才能成功。

平台以注册父设备作为 SIP 路由，XML DeviceID 为目标通道。目前仅接受注册父设备 From 和当前有效注册来源的响应，不猜测通道 From 兼容行为。关联同时检查 SN、父设备、目标通道和命令；合法但迟到或未知的结果回 200 后丢弃。该范围需要真实厂商设备继续验证，不能把受控 peer 当作摄像机验收。

## 状态与界面

复用既有 `handleDeviceQuery`、`pendingQueries`、原子 `messageSN`、`sendMANSCDP()` 和 `live.mu`。在途条目仅增加 `targetID`，区分用于路由/来源校验的父设备和 XML 目标；原 DeviceInfo/Status 两者相同。发送前登记、HTTP 返回 defer 删除，不关闭结果 channel。空数组通过响应中的可选列表指针区别于两个既有查询，不增加持久事实或运行状态机。

Qt 只增加三个实际控件：查询按钮、编号/名称列表、纯文本状态标签。复用既有 `control_context_`，无新的设备选择字段、缓存、timer 或自动重试。普通查询相互串行，切目标、A→B→A、刷新、换平台、关闭使旧回调失效。回调捕获父/通道编号值，不保存树节点。列表本身保存此次结果，不复制第二份缓存；异常列表不会留下部分有效项。

## 验证记录

| 检查 | 实际结果与证据 |
| --- | --- |
| HTTP 入口 RED | `endpoint-red.log`：旧版本路由 404，新参数检查应为 400 |
| Qt 控件 RED | `/var/tmp/media-server-qt-stage3-preset-tests-w8PH80ui/red-qtest.log`：2 PASS / 1 FAIL / 0 skip，失败为旧版本缺少 preset_query |
| 离线检查顺序 RED | `final-offline-red.log`：真实注销、注册过期、心跳过期清理通道后，旧顺序错误返回 404，而应返回 409 |
| 最终 Go 定向 | `final-query-directed.log`：13 个顶层测试 PASS，16.983 秒 |
| 最终 Go 完整 normal | `final-go-test-full.log`：signaling 33.718 秒、simulator 0.002 秒，PASS；首轮失败仍保留 |
| 最终 Go 完整 race | `final-go-race-full.log`：signaling 37.073 秒、simulator 1.008 秒，PASS |
| 最终 Go vet | `final-go-vet-full.log`：PASS，无输出 |
| 桌面 normal | `normal-ctest.log`：4/4 CTest，106 项 QTest、无失败/跳过，63.53 秒 |
| 桌面 ASan+UBSan | `asan-ctest.log`：4/4 CTest，106 项 QTest、无失败/跳过，63.30 秒 |

Go 使用 Go 1.27.1、`GOMAXPROCS=2`、`-p 1`、`-count=1`；normal 的 `CGO_ENABLED=0`，race 的 `CGO_ENABLED=1`。七个新增顶层测试与原七个 DeviceInfo/Status 测试均包含于全量测试；定向的 `TestDeviceQuery|TestPresetQuery` 为 13 个顶层测试，不包含另一个 `TestDeviceStatusRejectsInvalidSuccess`，后者由全量覆盖。覆盖真实 UDP 往返、空列表、字符串编号/空名称/GB2312、缺省与不符 Num、重复和非法项、早到 XML 后 SIP 拒绝、错误父/来源/目标/命令/SN、取消、并发跨通道、迟到响应与 pending 清理，以及实际生产 5 秒期限。

桌面四组 QTest 分别为 12、11、24、59（包含初始化/清理）。窗口原 30 项保留，新增 29 行覆盖仅通道可查、精确路径/空 body、字符串编号/空名称、字面文本、空列表、错误类型/重复项/不匹配结果、HTTP 400、Info/Status/Preset 互禁、A→B→A、刷新、换平台、关闭和直接析构的迟到成功/错误。Preset 网络超时在第二个独立窗口与原 Info 超时并行，保留原断言、不新增 10 秒等待，也未更改 CTest 30 秒外层预算。测试源冻结 SHA256：`543c88b01b373a0e246844eeedaf339af36a51a953ea45317bf246e21b53b9b6`。

使用 GCC 16.0.1 experimental、Qt 6.2.4、SDL2 2.0.20 和阶段一私有 FFmpeg 9 shared SDK；normal 为 `-O2 -g -DNDEBUG -Werror`，ASan+UBSan 为 `-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer`。`asan-ldd.txt` 确认实际加载 sanitizer runtime；没有关闭 leak detection 或增加 suppression。桌面独立构建不依赖 Boost.Context；外部 Qt/SDL/FFmpeg 库未全量插桩，不能扩称第三方内存安全全覆盖。

### 实际应用组合

`gui-run-03/` 和 `gui-run-03-complete.log` 记录最终 Go 源码下真实 `gb_monitor` ELF、独立 Xvfb/XTest 点击、Go HTTP handler 与受控 UDP SIP peer 的完整链路。父设备和 Catalog 由私有测试前置，不执行 REGISTER、媒体播放或物理动作。实际发出一条 PresetQuery（SN 1，父设备 34020000001320000001、XML 目标通道 34020000001320000002），界面显示 `camera-A / stage3-real-preset`、`301 / 空名称`；root 复核实际截图。pending 归零、平台注册 online 不变，4 个 GUI 检查全通过，组合测试 1.386 秒；应用与 Xvfb 均正常退出 0 并被回收。离线检查顺序修复前的 `gui-run-02/` 亦通过（1.324 秒），原件和源码副本保留。

- `gb_monitor` SHA256：`d70c196e5e781bfb143f18221497af9526406bb406e00a999b48d347a21712be`。
- `gui-run-03/result.json` SHA256：`72c650887a5f5a257e7b804e550d58022174c3746da9df9d91466d08a6dcd239`。

### 原始失败与限制

`go-test-full.log` 首轮仍在既有 `TestPushCredentialConcurrentConsumptionAcrossConnections` 报 SQLITE_BUSY，signaling 32.972 秒。之前阶段在原基线独立重复 30 次有 2 次同类失败；本轮没有修改 store 或断言，不把复验通过写成首轮通过。`endpoint-cli-error.log` 是 Go 命令误写 `-p1` 的工具调用错误，不是产品 RED。

`gui-run-01/` 是私有全屏 OCR 将可见 `query test` 误读后的等待失败，未发点击或 UDP 查询；应用受控终止、Xvfb 正常回收。仅修改私有脚本为侧栏 ROI 放大 OCR 后，run-02 保留相同最终编号/名称显示断言并通过；没有修改生产布局或放宽产品断言。

最终 GUI 的 `gui-run-03.log` 是首次私有源码副本遗漏 `go:embed web/*` 的 setup 失败，尚未启动应用或 SIP。补齐私有副本的原始 web 资源后，以独立 `gui-run-03-complete.log` 保存成功；原失败不覆盖。

Claude `round-19.json` 发现的离线错误码顺序是实际可达缺陷：注销、注册过期、心跳过期均会移除 Catalog，原顺序因此先返回通道不存在。三条真实路径先建立 RED，再只将在线检查移至通道检查之前，保持 stopping 优先；最终全量 Go 和实际应用组合在修复后通过。没有增加离线状态或兼容分支。Qt 源码未变，不重跑相同桌面套件冒充新增证据。

本轮证据目录：

- Go：DEV `/var/tmp/media-server-qt-stage3-preset-92XuAF4R/`。
- 桌面 normal/ASan+UBSan：DEV `/var/tmp/media-server-qt-stage3-presets-main-UakD2P0j/`。
- Claude 只读设计与最终审查：DEV `/tmp/qt-client-plan-claude-JuZHjy/`。

原始日志及私有组合副本保留在证据目录，不提交原始授权或网络报文。不继承旧的 16 路、第二机或公开发行许可结论。

## 最终复审与清理

DEV Claude Code 只读复审 `round-20.json` 确认离线检查顺序修复、父设备不符时明确拒绝的身份边界及最终验证证据，没有剩余可达 P0/P1，可以提交本查询子集并仅推送开发分支。PTZ、预置位写操作和物理设备验收不在本次通过范围。

本轮仅清理独立重复桌面构建的 `normal/CMakeFiles`（约 23 MiB，可由原源码和配置重新生成），路径为 `/var/tmp/media-server-qt-stage3-preset-tests-w8PH80ui/normal/CMakeFiles`。清理前核对真实路径、CMakeCache 所属及无使用中进程；保留该目录的测试 ELF、Testing 原始记录、源码、RED/GREEN 日志和报告。最终 normal/ASan 构建保留供后续增量验证，不清理历史证据或其他 Agent 资源。
