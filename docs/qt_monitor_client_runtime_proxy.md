# Qt 内部运行包的 libproxy 动态依赖补齐

2026-10-18，以 clean `4f4cd12` 为初始基线。第二机16独立源验收的原始运行包通过静态 `ldd -r` 和逐文件SHA，却在真实Qt启动约0.566秒后加载TEST系统的 `network_networkmanager.so`。原采样器的包外非glibc映射检查正确终止本轮；尚未进入共同30秒媒体窗口，不能称为解码性能失败或16路通过。

## 根因与最小修复

当前Ubuntu `libproxy1v5` 为0.4.17-2，Qt Network静态依赖该库；模块不在DT_NEEDED闭包中。锁定[0.4.17源码](https://github.com/libproxy/libproxy/blob/e78a5ccfe0a2340f2c73e419767f8492ffc2787a/libproxy/proxy.cpp#L149)显示 `PX_MODULE_PATH` 替换编译期模块目录，并非追加路径；[module_manager](https://github.com/libproxy/libproxy/blob/e78a5ccfe0a2340f2c73e419767f8492ffc2787a/libmodman/module_manager.cpp#L272)按完整文件路径dlopen。只设置LD_LIBRARY_PATH不能重定位这些模块。

打包器在确实依赖libproxy时补入当前版本的NetworkManager、GNOME配置两个模块及 `pxgsettings`，复用现有依赖闭包、版权文件、manifest、SHA与最终ldd-r检查。启动器显式设置包内 `PX_MODULE_PATH` / `PX_GSETTINGS`，不禁用代理或放宽映射检查；未新增播放器、C++/Go状态或业务接口。

[GNOME模块](https://github.com/libproxy/libproxy/blob/e78a5ccfe0a2340f2c73e419767f8492ffc2787a/libproxy/modules/config_gnome3.cpp#L121)先stat原helper路径，随后交给shell执行，不能简单将引号放进变量。启动器在上游实际启用条件（GNOME会话变量存在，或DESKTOP_SESSION精确为gnome/mate）下，拒绝含空格或shell元字符的安装路径并明确报错；非GNOME模式保留空格路径重定位。没有修改第三方源码或通过清空会话变量降级。

## 定向回归

新增手工运行的 `desktop/tests/package_runtime_test.py`：

```bash
INTERNAL_RUNTIME_DIR=/absolute/path/to/new-runtime \
  python3 desktop/tests/package_runtime_test.py -v
```

显式缺少包路径会失败，不隐式skip，也不让普通CTest依赖本地生成包。检查真实ELF/许可/manifest SHA，实际通过ctypes构造并释放包内libproxy factory，要求真实NetworkManager模块来自包内；空目录仅作路径替换负对照，不进入产品策略。另检查启动器覆盖继承路径，以及GNOME/MATE/空值GNOME会话变量下的不安全路径拒绝。临时stub只检查启动器环境，不冒称真实Qt媒体验收。

旧包原始RED和新包实际完整CLI、重定位/逐文件SHA及回归日志分别保留在DEV：

- `/var/tmp/qt-runtime-proxy-4f4cd12-6IdDVQc9/`：首版3项GREEN及旧包RED。
- `/var/tmp/qt-runtime-proxy-final-4f4cd12-D2RNfLG7/`：最终旧包RED退出1，新包完整打包退出0；移至含空格路径后4项测试零失败/跳过，退出0、0.043秒，逐文件SHA检查退出0。manifest如实标记dirty，不能冒充clean发行包。

跨机原始失败：DEV `/var/tmp/qt-second16-4f4cd12-TngcgwrF/run-01/` 是私有监督器误用wildcard地址；`run-02/` 是本次动态依赖RED，TEST副本 `/var/tmp/qt-second16-run-4f4cd12-TngcgwrF-run02/`。第二轮Qt经身份核验终止、PID/进程组消失，16个发布源自然退出0，全部本轮配置删除、服务退出0及TCP/SIP UDP端口重绑。原文件与SHA保留，不用离线GREEN改写原FAIL。

## 边界

该增量不是完整16路验收；修复包还须在第二机以原媒体/窗口/阈值再次验证实际Qt映射和持续解码。D-Bus/NetworkManager服务、GNOME schema/用户设置及完整GNOME/GIO/PAC行为未由本次headless factory测试覆盖。字体、显示/声音服务仍由宿主提供；公开分发许可保持BLOCKED。未重跑无关性能或Scale回归，未修改任何既有覆盖或健康断言。
