# Qt 同会话录像倍率操作

## 范围

从 `46cba83` 的[输入节奏呈现增量](qt_monitor_client_stage4_scale_presentation.md)继续，仅接通主窗口操作，不修改 signaling、媒体服务器或解码/呈现算法。

- 录像控制行增加0.25、0.5、1、2、4五档请求值，默认1。选择本身不发送，必须点击“请求倍率”。
- 按钮调用当前选中格既有 `scale_record()`，复用 `record_seek_ready()`。请求进行中、暂停、无媒体、EOF或停止时禁用；原 owner/控制互斥及取消语义不变。
- 切换画面将输入重置为默认请求值，不将它显示为已生效或设备实际倍率。非选中格的迟到响应只触发按当前格刷新，不覆盖输入。
- 只增加两个实际控件指针，不增加连接状态、倍率事实、timer、HTTP接口或后台任务。

## 验证

DEV私有证据：`/var/tmp/qt-scale-ui-46cba83-OEDXEYcK/`，冻结输入 `source-validation.sha256`，命令 `validate.sh`。复用前一增量的两套构建、SDK和素材，不重新复制依赖。

`monitor_window_test` 复用原双画面真实PS夹具，原seek场景保留，另加scale数据行：真实TCP核对两格对应的POST路径/JSON、五档值、显式点击、请求忙时重复点击抑制、暂停/继续/停止、选中格切换与迟到204输入隔离；两格合计仍只有两次创建和两个媒体GET。禁用控件单独覆盖等待媒体的状态。

这些是实际Qt Widgets与HTTP/PS运行，不是物理鼠标或相机倍速验收。UI用例没有直接观察owner/decoder对象identity及跨平台owner隔离；后者仍由既有有限回放生命周期测试覆盖。请求4倍受理也不代表已经验证实际4倍呈现。

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| Normal构建 | PASS，退出0 | `normal-build-01.log/.exit` |
| Normal窗口CTest | PASS，1/1，167项QTest无失败/跳过，71.59秒，退出0 | `normal-monitor-01.log/.raw.log/.exit` |
| ASan+UBSan构建 | PASS，退出0 | `asan-build-01.log/.exit` |
| ASan+UBSan+LSan窗口CTest | PASS，1/1，167项QTest无失败/跳过，73.62秒，退出0 | `asan-monitor-01.log/.raw.log/.exit` |

前一增量Normal及ASan+UBSan+LSan完整桌面各10/10、469项通过，绑定其六份源文件；不把历史完整结果冒充本UI提交重新执行全套。当前只针对受修改影响的窗口组回归。构建前后核对本轮三份输入SHA，ASan未关闭LSan或添加抑制；外部Qt、SDL、shared FFmpeg不是全量插桩。独立只读复审未发现阻断问题。

## 后续边界

慢速、实际4倍、非整数速率、断供恢复、暂停与倍率组合和Qt到真实大华的双轨呈现仍须继续；宽带AAC变速音质及物理声画限制见呈现文档。五档请求接口不是完整功能验收，七阶段仍未整体GO。

本轮未删除任何历史证据或其他Agent资产。清理仅限 `/tmp`、`/var/tmp` 内可确认属于我们且不再需要的产物。
