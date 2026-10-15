# Qt 阶段三：预置位设置、调用与删除

基线 `5f952f23e3e80e4ab0631cc6d0bb395aa5242128`，只在 `feature/qt-monitor-client` 开发。该子集不是整个阶段三或七阶段完成；物理设备验收仍待确认。

## 单一协议路径

复用 `POST /api/devices/{device_id}/channels/{channel_id}/ptz`、DeviceControl XML、原注册设备路由快照和既有 SIP MESSAGE 事务。新增三种命令：

```json
{"command":"preset_goto","preset_id":1}
```

`preset_set` / `preset_goto` / `preset_remove` 对应 [GB/T28181—2016](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf) 附录A.3.4的81/82/83，编号为整数1–255、0保留，第5字节0，第6字节编号；共享原8字节校验和，不经过方向低位mask。设置/调用/删除1号分别为 `A50F008100010036`、`A50F008200010037`、`A50F008300010038`。

预置位命令不得携带speed，旧方向/变倍/STOP不得携带preset_id，包括null；RawMessage仅用于区分字段缺省及显式null，不新增业务状态或转换模型。选定参数只解析为一个整数，小数、字符串、越界拒绝；旧STOP speed缺省、0及原有null→0语义保留。父设备、在线、Catalog、停止中校验不变。

成功HTTP200包含父/通道/command/preset_id、`accepted:true`、`physical_result:unconfirmed`，没有start/stop字段。原方向及STOP实际输出不变，Stop仅增加omitempty，实际均有值。预置位SIP事务最多1.5秒，失败返回502 `preset_control_unconfirmed`；独立STOP失败仍优先 `ptz_stop_unconfirmed`。SIP受理不是保存、到位、删除或物理停止证明。

## owner 和停止时序

继续唯一 `ptzOwners map[liveKey]CancelFunc`，不新增移动状态、DB字段、registry、代际或第二锁。预置位与点动共用同通道互斥、跨通道并行。

预置位收到SIP200且锁内检查尚未取消后，不等300毫秒、不自动STOP，而是在原live.mu内删除本次owner，随后输出HTTP响应；它不以客户端已经收到HTTP200为前提。手动STOP看到既有owner时也在同锁内cancel，返回202 requested。两者线性化：要么STOP先取消且原owner在本地事务结束后独立STOP，要么原owner先释放，由新无owner STOP自己发送；不能202却无人尝试STOP。取消/不确定结果仍独立后台1.5秒STOP，原注册快照不变。清理只有两个互斥出口，不能再由通用defer删除一次、误删后来的owner；defer只负责取消本次context。

即使设备已经返回SIP200，若成功分支锁内检查之前被STOP、HTTP断开、注销或停机取消，仍尝试STOP并报告`preset_control_unconfirmed`（STOP本身失败时优先`ptz_stop_unconfirmed`）。设备可能已经保存或删除，未确认不是未执行，不能自动重试或恢复旧列表。

正常goto受理后owner释放，并不表示物理运动结束。之后的HTTP断开（包括尚未收到响应）、注销、Qt退出和平台停机不再追踪该移动；用户可显式STOP或新点动，后者结束时的STOP也可能中断原goto。标准控制消息没有提供到位进度，不能为了显示“正在移动”新增猜测状态。设备是否支持全零STOP中断goto尚须实测。取消/失败后的STOP不补偿已经生效的设置/删除，也不保证网络乱序、丢包或kill -9时物理停止。

## Qt确认及编号

查询继续显示任意设备字符串ID。设置由1–255 SpinBox明确输入；调用/删除只能选择可原样编码的规范ASCII十进制1–255，`001/+1/1000/camera-A`等不可隐式转换；不改查询解析边界，不增加名称写入。

设置/删除在新control_context建立后显示PlainText非模态确认框，默认No。确认回调绑定该context并捕获父/通道/数字值，QObject销毁关闭对话框，QPointer守住销毁期间finished重入；A→B→A也不能复活旧确认。STOP仍可使用，确认取消零请求。普通控件及列表在待确认/请求时禁用，选择事件只依列表实际enabled与选中编号派生调用/删除，不解除忙碌。新增成员仅实际控件，selected_preset_id在显示使能和发请求处复用。

设置/删除确认后发送时清空旧列表（即使后来响应失败也可能已生效），提示手动查询；不自动查询或乐观增删。调用不清列表，不宣称到位。HTTP200、identity、command、真实数字ID、布尔accepted及physical_result全部校验。切目标/平台/刷新/STOP/退出沿用原context隔离，不把客户端销毁误认为撤回网络控制。

## 验证与证据

| 验证 | 实际结果 |
| --- | --- |
| Go定向协议与生命周期 | PASS，18项顶层测试，16.089秒；含9项预置位、7项原PTZ、2项真实run |
| Go全量test / race / vet | PASS，signaling 51.212 / 54.167秒，simulator 0.002 / 1.011秒；vet退出0 |
| 桌面普通构建与CTest | PASS，4/4，81.99秒，191项QTest，无失败或跳过 |
| 桌面ASan+UBSan构建与CTest | PASS，4/4，82.82秒，191项QTest，无失败或跳过，无sanitizer诊断 |
| 实际Qt→HTTP→UDP组合 | PASS，私有GUI run04应用3.023秒、Go组合3.226秒；不是物理摄像机动作证明 |
| 物理设置保存/调用到位/删除/STOP | BLOCKED，硬件条件待确认 |

Go标准字节、整数/字段存在性、旧响应逐字、成功无自动STOP、同通道互斥/跨通道、真实HTTP断开、Digest注销、serve停机、12轮完成/STOP竞争及owner清空均有断言。Qt新增41行，窗口完整144项；包括非模态默认No/PlainText、确认取消零请求、ABA/STOP/析构隔离、严格响应、编号边界和真实默认10秒HTTP超时。其余12+11+24项为平台请求、解码与播放生命周期回归。

RED保留原编码不支持和API400、旧Qt缺少实际控件。原`directed-01.log`漏Content-Type及`directed-final.log`清理早于UDP回调的测试夹具失败均保留；后者只增加START/STOP回调完成屏障，不忽略错误。私有GUI前三轮失败均保留：run01点击列表空白，run02无WM的Xvfb中键盘未确认，run03结果文案换行令按钮行上移，调用点击未命中；应用均由TERM退出-15并reap，私有Xvfb正常回收。只修私有脚本真实鼠标和截图定位、可用性有界屏障，不改生产或断言，不覆盖失败。

run04真实鼠标先取消设置（零UDP），再次确认设置、查询、调用、确认删除。受控peer依次收到SN1设置、SN2查询、SN3调用、SN4删除，三条控制编码逐字正确、无自动STOP；owner=0、pending=0、在线注册事实未改变。应用WM_DELETE正常退出0、私有Xvfb退出0并均reap；最终中文截图由主代理实际查看，显示“控制已受理，物理结果未确认；请手动查询列表确认”，不宣称删除生效或到位。图像及SHA位于`gui-run-04/result.json`，完整日志`gui-run-04.log`；未启动媒体或访问硬件，此组合不替代解码/物理动作验收。

Qt窗口原CTest外层45秒，新增工作后实测44.781秒，仅余0.219秒；据此改60秒，生产HTTP10秒期限及原断言不变。最终normal窗口44.57秒、sanitizer45.39秒。ASan实际链接libasan/libubsan，编译使用address,undefined及frame pointer，未关闭leak检测；Qt、SDL及共享FFmpeg不是完整插桩库，该结论不扩大为所有第三方或整个媒体服务器ASan全量通过。

- Go RED/GREEN和私有GUI：DEV `/var/tmp/media-server-qt-preset-control-sIcW6NEo/`。
- Qt RED/GREEN：DEV `/var/tmp/media-server-qt-stage3-preset-control-tests-PQLhWkD7/`。
- 最终桌面构建：DEV `/var/tmp/media-server-qt-stage3-preset-control-main-3OjFqtkm/`。
- Claude设计讨论：DEV `/tmp/qt-client-plan-claude-JuZHjy/round-24.json`。
- Claude独立代码复审：DEV `/tmp/qt-client-plan-claude-JuZHjy/round-25.json`，无可达P0/P1；要求的GUI取消零报文、完整顺序、owner/pending归零、正常退出及文档补齐均完成。只允许提交推送本开发分支，不表示阶段三物理动作GO。

最终GUI ELF SHA256：`fd911a728388f89f6b8c213bc084c2f0209c5e121f2eb04af4063dac556c0304`。普通/ASan测试及GUI ELF校验和见最终构建目录`build.sha256`；源码、编译参数、CTest原始输出、失败日志保留，不提交原始授权信息。按用户阶段清理要求，仅删除本轮测试增量目录`red-build/CMakeFiles`及`normal/CMakeFiles`中可重建对象，25200+28072 KiB；配置详情已另存，RED/GREEN ELF、源快照、Testing和日志保留。最终普通/ASan构建保留用于后续回归，未清理他人资源。

不访问历史摄像机/TEST，不继承厂商历史PASS。真实保存、到位、删除及STOP中断goto均BLOCKED。公共发行许可仍未闭合，内部功能构建不代表可公开分发。
