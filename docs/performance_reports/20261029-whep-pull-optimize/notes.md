# 工作记录（最终报告素材）

## R1 转码成本定位（TEST 主机 CPU 7，perf stat cycles:u）

- msbench 链接真实 `media_server::audio_transcoder`，与 whip_media_receiver 同配置（Opus 48k/2 → AAC LC 48k/2，128 kbps，cutoff 20000）。
- 输入 `perf.mka`（float 输入 libopus）：19.9 ms CPU/音频秒，与 aacbench ff 一致，20 路交错不变。
- 输入 `perf_fixture_720p30.flv` 经真实上游链（AAC → media_server Opus egress → AAC）：32.2 ms/s。
- 输入 `perf_s16.mka`（s16 输入 libopus，与上游 egress 相同）：32.3 ms/s ≈ 90 Mcyc/s，复现服务端实测 ≈90 Mcyc/s/会话。
- 结论：服务端 1.55× 差异来自上游 Opus 信号本身（s16 量化后的低电平噪声让 twoloop 量化搜索更重），不是 audio_transcoder 实现开销。cutoff=20000 的 libopus 输入不影响。

## 编码器成本（perf_s16.mka，360 音频秒，gcycles 含解码 3.15）

| 模式 | ms/音频秒 | gcycles | 编码 Mcyc/s |
| --- | --- | --- | --- |
| decode only | 2.27 | 3.15 | – |
| ff twoloop（现状） | 29.6 | 32.45 | 81.4 |
| ff aac_coder=fast | 12.4 | 14.08 | 30.4 |
| ff aac_tns=0 | 28.8 | 31.67 | 79.2 |
| ff aac_pns=0 / aac_is=0 | 29.6 | 32.6 | 81.8 |
| ff cutoff=0（默认带宽） | 26.8 | 29.51 | 73.2 |
| ff cutoff=16000 | 25.8 | 28.47 | 70.3 |
| fdk afterburner=1 | 10.9 | 13.28 | 28.1 |
| fdk afterburner=0 | 5.0 | 6.48 | 9.3 |

## 客观音质（tools/quality/nmr.py，简化 NMR，越低越好；6 首私有歌曲各 30 s + perf 夹具）

total NMR dB（disturbed frames %）：

| 素材 | ff twoloop | ff fast | ff cutoff=0 | fdk ab=1 | fdk ab=0 |
| --- | --- | --- | --- | --- | --- |
| perf | -21.8 (0.1) | -37.9 (0) | -28.0 (0) | -27.9 (0) | -25.5 (0) |
| song1 | -4.6 (84) | -3.7 (100) | -5.2 (67) | -9.4 (4.6) | -8.9 (5.2) |
| song2 | -2.9 (91) | -3.2 (95) | -3.7 (82) | -7.5 (35) | -6.8 (43) |
| song3 | -5.7 (54) | -4.5 (92) | -6.1 (51) | -10.9 (3.7) | -10.3 (3.5) |
| song4 | -2.2 (71) | -2.0 (95) | -3.6 (52) | -7.7 (15) | -6.9 (22) |
| song5 | -1.9 (97) | -1.4 (100) | -2.8 (87) | -6.1 (52) | -5.6 (55) |
| song6 | -2.5 (90) | -1.8 (99) | -3.3 (80) | -6.8 (42) | -6.3 (45) |

- ff fast：真实音乐上 5/6 首 NMR 更差、扰动帧显著增加 → 降质，拒绝。
- cutoff=0：FFmpeg 默认带宽低于 20 kHz，等于收窄音频带宽；歌曲素材是 MP3（本身多在 16–19 kHz 低通），指标无法反映高频损失，属于降质，拒绝。
- 局限：歌曲源是有损 MP3，NMR 为简化模型，只用于同参考下编码器相对比较。
- fdk（ab=0/1）：全部素材 NMR 优于现状 3–5 dB，CPU 降为 1/3–1/9；许可证与当前 GPLv3 FFmpeg 构建不兼容，仅作隔离实验。

## 构建参数

- FFmpeg 以 `-march=x86-64-v3` 重编：perf_s16 ff 模式 32.51 → 31.88 Gcyc（-2.0%，两轮一致），输出 AAC md5 完全相同。收益不足以换取 AVX2 部署要求，拒绝。
- 先前 profile 热点：search_for_quantizers_twoloop 28.5%、quantize_and_encode_band_cost_* ≈30%、cbrtf 6.6%，以标量 C 为主，编译参数难以显著改善。

## 许可证路径（fdk）

- media_server 只调用 FFmpeg 的 Opus/AAC 编解码、swresample、swscale；x264/x265/vpx/aom/dav1d/rsvg 只是随 ~/ffmpeg901 静态库链入，使二进制成为 GPLv3。
- FFmpeg 9.0.1 configure：`EXTERNAL_LIBRARY_NONFREE_LIST` 含 libfdk_aac，但只在 `--enable-gpl` 时要求 nonfree（configure:4833）。实测 `--enable-libfdk-aac --enable-libopus`（无 gpl）配置通过，License: LGPL 2.1+。
- 即：为 media_server 单独构建精简 LGPL FFmpeg + libfdk-aac 在 FFmpeg 许可层面可行；FDK 自身许可与 AAC 专利由用户/法务决定。
- 实验构建（已清理）：ffmpeg-fdk（GPL+nonfree，仅测试用）、worktree 分支 exp/fdk-aac（audio_transcoder 优先 libfdk_aac + GLOBAL_HEADER），ELF f8e7e6a7…；本地 lifecycle（fdk-r1）全部通过。

## FFmpeg aaccoder cbrtf 查表（隔离实验）

- `libavcodec/aaccoder.c:140` ESC 码本每个系数 `c*cbrtf(c)`（源码注释 FIXME: slow）。补丁 `tools/ffmpeg/aaccoder-pow43.diff`：首次用 ESC 时 ff_thread_once 预计算 8192 项 `c*cbrtf(c)` float 表。
- 同一 FFmpeg 树补丁前后，perf_s16 ff 模式三轮：32.55/32.72/32.57 → 28.36/28.26/28.29 Gcyc（-13.2%；扣除解码后编码 -14.6%），输出 AAC md5 相同（23bc4a1f…）。
- 打补丁后热点：search_for_quantizers_twoloop 34%、band_cost_ESC 13.4%、UQUAD/UPAIR/SQUAD 各 6–7%，均为平摊浮点运算，没有第二个同类低垂点。
