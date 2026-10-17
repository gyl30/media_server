# 第二机安装版外部播放补验

2026-10-17，工作树基线98592ce。仅补安装版程序的外部播放操作、数字声音和正常关闭，不重复此前已通过的窗口探针单/四路测试，未修改生产代码。

## 受测资产与方法

使用clean579227a内部归档，manifest的git_head为 `579227a8483acf9e577e6d9e5285eb7c46c303af`、dirty=false。归档SHA `4c21465556b288ac2574f25962108ed2972e4d959c1ea8a4e10617e1cb3d2d9e`，安装版 `gb_monitor` SHA `7f6c544b49d3e30d0769cce2799a24f304ff3d4f463898c44694a258e77c0e49`。桌面源码此后无变化；Go私有launcher来源347decb，生产信令源码此后亦无差异，不能将复用ELF称为98592ce新构建。媒体仍为冻结9abe7ac6版本。

TEST实际执行 `runtime/run.sh`，独立QSettings指定平台和单画面。DEV仅通过私有认证Xvfb及SSH loopback6198发送外部XTest鼠标事件和WM_DELETE，不调用Qt测试信号或内部播放接口，不使用用户物理桌面。真实大华注册/目录就绪后，双击父设备、通道，再点击声音按钮中心；私有Pulse按实际Qt PID归属sink-input并录制PCM。

## 修正后结果

DEV `/var/tmp/qt-installed-corrected-98592ce-v98mvuwa`，TEST `/var/tmp/qt-installed-corrected-98592ce-etmK9r0d`，受测run-03。安装程序PID2234664，结果 `PASS_CONTROLLED_DIGITAL`。

- 主代理亲自核对两张截图：相隔21.4937秒，解码/显示从118/115增长到656/648；真实相机画面持续更新。声音点击后明确为“关闭声音”。
- Pulse sink-input归属PID2234664；48kHz、双声道S16数字录音共3054395帧，2572794个非零int16采样，peak70。PCM SHA `0ece17f8fdfedcf92c9695bb4a69d087b43cd603bb41914ba53337fc705f2ae4`。这些只证明数字输出，不代表物理扬声器或听感。
- 观察到的Qt、FFmpeg、SDL和XCB插件映射均来自该包。外部WM_DELETE后Qt退出0，parec退出0，PID消失；随后平台、媒体、Pulse均请求停止并退出0，`live_sessions=0`、`broadcasts=0`。
- 本轮Xvfb/SSH隧道全部回收，两机临时认证文件删除。只收普通证据文件并逐项校验SHA，不复制活动socket或凭据，不删除历史资产。

## 原失败保留

08:31额度中断的DEV `/var/tmp/qt-installed-play-919a6ac-claTKHTT`、TEST `/var/tmp/qt-installed-play-919a6ac-k0cJOKjA`原GUI超时保持INCOMPLETE；恢复时按精确PID/starttime清理遗留自有Xvfb/隧道，旧日志和实际画面保留。

12:24补验的DEV `/var/tmp/qt-installed-resume-919a6ac-1A6ubf1b`、TEST `/var/tmp/qt-installed-resume-919a6ac-H8HHxFaO`也保持INCOMPLETE。点击记录x845,y757落在全屏按钮右沿，实际声音按钮x852..931，截图仍为“开启声音”。主代理曾误读为“关闭声音”，经独立复核和重新看原图纠正；原全零PCM及无sink-input不能归因于产品音频故障。独立复验只把点击改为x890,y757，没有修改生产、包或音频门槛。

新根首次run-02在启动任何子进程前因归档顶层目录未改名而失败；保留该INCOMPLETE，正确重命名后另建run-03，不覆盖失败记录。没有新广播、相机SET或音量变更。

本项关闭的是第二机安装版外部单路实际播放/数字声音/正常关闭缺口；第二机物理显示和声音、16路、输入法、广播/对讲及公共分发许可仍未验收，完整平台仍未GO。未重跑桌面CTest、Go全量或sanitizer。
