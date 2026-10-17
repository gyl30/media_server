# 内部运行包的编译器与GCC来源补证

2026-10-17，基线53b0e4d。打包器原来只识别 `CMAKE_CXX_COMPILER:FILEPATH`，当前真实CMake缓存却使用 `STRING`，导致 manifest 的 `compiler_version` 误记为 `NOT_RECORDED`。现同时接受这两种CMake类型，仍执行记录路径的 `--version`，不按目录名猜版本。

通过完整打包CLI建立真实RED/GREEN：修正前包生成成功，但版本断言失败；修正后重新生成独立包，版本字段与 `/usr/bin/g++-16 --version` 完全相同，包内依赖检查通过。实际编译器是Ubuntu GCC 16.0.1 experimental。未改变播放器、SDK、媒体服务器或Go，也未重跑桌面CTest、Go全量、ASan/UBSan。绿色包准确标记53b0e4d+dirty，不冒称clean发行版。

私有证据 `/var/tmp/qt-package-provenance-53b0e4d-P04YWoZ6` 保留旧脚本、完整CLI/日志、RED及GREEN manifest、运行包、配置原件、SHA和签名验证结果。

## GCC运行库来源

编译器和链接运行库分别追溯；当前包的三份自定义GCC运行库与 `/usr/local/gcc-16.2/lib64` 及 `/home/gyl/opt/src/gcc-16.2.0-build` 内对应产物逐字节一致：

| 运行库 | SHA256 |
| --- | --- |
| libstdc++.so.6 | `55fbc153e014ca1b59b8b8d124018e62ba9ef394b1ebd84d9885954573ca5ce0` |
| libgcc_s.so.1 | `67f6582db7e1a36e9d5e5902f4fd97b7d30a76b64e670a8a8324445440e61a58` |
| libgomp.so.1 | `1f25098e078fe4c820733156f868474cff45699981733afb12e4b93a5c18b076` |

三库ELF `.comment`记录GCC16.2.0；构建目录 `config.status` 指向 `../gcc-16.2.0`。本地源码 `gcc/BASE-VER` 为16.2.0，归档 `/home/gyl/opt/src/gcc-16.2.0.tar.xz` SHA `e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e`，附带签名经本地GNU keyring的gpgv检查退出0。归档的BASE-VER、COPYING3、COPYING.RUNTIME与源码树逐字节一致；未执行整个源码树等价检查或GCC重建，不把有限核对扩大为可重现构建证明。

本轮只补证据，没有把自定义GCC运行库冒记为系统dpkg包，也没有把实际程序编译器改成16.2。打包器的 `unresolved_license_sources` 仍如实保留三库，当前运行包仍仅供内部验证。

## 分发边界

libstdc++官方说明其代码使用GPLv3及GCC Runtime Library Exception；GNU FAQ同时区分程序链接与运行库自身的二进制分发，后者仍有源码交付要求。依据见[libstdc++许可](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html)、[GNU例外FAQ](https://www.gnu.org/licenses/gcc-exception-3.1-faq.en.html)。本轮未以例外替代全部分发义务。

`public_distribution=BLOCKED` 保持不变：项目自身许可证尚未由用户决定，完整依赖源码交付和发行材料也未验收。源码/构建资产存在不等于已随包完成交付；本轮未发布、部署或自动授予项目许可证。
