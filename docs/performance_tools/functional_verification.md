# 功能、浏览器、codec及国标回归入口

这些入口属于短时正确性回归/界面检查，不是正式性能容量数据。每项说明均以当前源码为准。**帮助退出0≠E2E可用**：部分脚本仍依赖旧无TokenURL或旧API返回字段。本轮不恢复兼容URL、不关闭认证、不把未执行的用例写成PASS。

## 公共依赖与证据

- Python标准库；浏览器工具另外需要Playwright Python包和实际Chrome executable。`bench/requirements.txt`目前仅声明aiohttp，不代表已包含Playwright。缺少浏览器/库标BLOCKED，不能伪造getStats。
- fixture为真实可解码媒体；GB H264文件需要Annex-B及AUD，PCMA文件为8000Hz mono raw A-law。真实摄像机素材不得作为公开fixture。
- Go signaling/simulator必须独立构建、显式传入；runtime bundle不自动意味着这些资产已经存在。路径和版本按manifest核对。
- 每项output使用新目录；日志/SDP/URL/result/commands可能包含Token等敏感内容。不要公开提交原始结果。
- 大多工具自主启动媒体服务/信令/模拟器/Chrome，再在finally关闭自有Popen/浏览器；父runner被强杀不保证同样cleanup。使用独立端口和本轮进程身份，不全局kill。
- 本页的`--help`本轮在DEV实际执行过；媒体验证示例除非本轮结果单独注明，一律DOCUMENTED / NOT_EXECUTED / REQUIRES_SERVICE。工具的BLOCKED说明优先于原脚本中的历史PASS文字。

## `browser_webrtc_verify.py`

用途：Chrome真实WHIP编码或WHEP解码；检查ICE/DTLS、codec、视频帧、音频采样增长、DELETE。它的原生RTCPeerConnection/getStats逻辑有实际价值，但共享`lifecycle_verify.Run`仍用旧未授权入口，因此当前**BLOCKED / NOT_COMPATIBLE_CURRENT_AUTH**。

```bash
python3 bench/browser_webrtc_verify.py --help
```

参数：位置方向`whep|whip`；必需`--build-dir --fixture --output`；`--ffmpeg /home/gyl/bin/ffmpeg`、`--browser /usr/bin/google-chrome`、port-base42930、duration30。`--resume-video`仅用于WHIP暂停/恢复发送；`--expected-video h264|h265`默认h264，`--expected-audio opus|pcma|pcmu|none`默认opus。WHIP只允许当前Chrome H264/Opus编码组合。

依赖Chrome、Playwright、FFmpeg、server。原脚本自启server、publisher/browser及WHIP→RTSP解码probe，输出offer/answer.sdp、browser统计、FFmpeg进展日志/result/commands；成功不仅connected，还要求媒体增长和解码codec符合预期。真实失败通过异常及result FAIL报告，cleanup关闭Chrome/peer并尝试DELETE，server/publisher由Run关闭。

暂不提供误导性的“当前可用正常E2E”完整命令。native替代功能smoke用`perf_run.py --input whip --outputs whep:1 --duration 10 --progress-interval 2 --output "$PERF_NEW_OUTPUT"`，但**native成功不等于Chrome解码成功**。浏览器首次正式验收必须使用当次独立签发的真实URL，通过现有Web或下面的原生最小检查，不篡改offer。

自行管理输入的`perf_run`使用测试SignalingStub验证媒体鉴权边界，不是Go signaling E2E。真实Go授权验收使用`--input existing --signaling-url`及本代sources文件；GB SIP REGISTER/Catalog另行验证。

### 可复用的原生Chrome统计检查（不是新runner）

在本轮管理页面/已有Playwright page中，复用已经建立并由现有页面负责关闭/DELETE的原生PeerConnection。每viewer必须使用独立真实`whep_url`；不要改写浏览器SDP能力。以下仅采集统计，不另建连接或引入第二套创建/清理流程。

```javascript
async function mediaStats(pc) {
    const rows = [...(await pc.getStats()).values()];
    const track = kind => {
        const inbound = rows.find(x => x.type === 'inbound-rtp' && (x.kind || x.mediaType) === kind);
        const codec = inbound && rows.find(x => x.id === inbound.codecId);
        return inbound && {codec: codec?.mimeType, bytesReceived: inbound.bytesReceived,
            framesDecoded: inbound.framesDecoded, totalSamplesReceived: inbound.totalSamplesReceived};
    };
    return {connectionState: pc.connectionState,
        dtlsConnected: rows.some(x => x.dtlsState === 'connected'), video: track('video'), audio: track('audio')};
}
```

此片段为DOCUMENTED / NOT_EXECUTED，不是已验收工具或完整异常清理框架。实际用例必须在有界15秒等connected后至少采两次stats（间隔例如5秒），断言video.framesDecoded和bytesReceived增长；双轨还断言audio.totalSamplesReceived/bytesReceived增长；保存codec、SDP、时间及原始stats。结束或任何失败必须在finally `pc.close()` 并对原Location DELETE，保留HTTP结果；POST返回201但setRemoteDescription失败也必须DELETE。不要只检查connected，不伪造能力，不在非secure origin尝试WHIP getUserMedia。

## `codec_interop_verify.py`

用途：单个真实codec组合通过多个协议输出做FFmpeg解码和PTS检查。当前依赖旧Run/publisher、无Token输出URL，**BLOCKED / NOT_COMPATIBLE_CURRENT_AUTH**。

```bash
python3 bench/codec_interop_verify.py --help
```

必需publisher=`rtmp|rtsp`、video-codec=`h264|h265`、audio-codec=`aac|g711a|g711u|none`、build-dir、output。rtsp-publish-transport默认tcp（也可udp），duration3、port-base43930、FFmpeg同上；`--fixture`可复用，否则生成320×240/20fps测试素材。`--outputs`支持rtmp/rtsp/http/hls/gb_udp/gb_tcp_active/gb_tcp_passive，默认全部；不支持的codec/protocol组合不能预设应PASS。

输出fixture-command、decode进展、showinfo/ashowinfo、PTS count/first/last/non_monotonic、resources/result/commands。历史成功规则包含退出0、实际video/audio解码、多于一个PTS、单调性，另有约2GiB RSS/5.4core短时保护；这些是功能测试保护，不是统一server容量边界。每probe有duration+20s截止、socket超时；finally结束Run。当前替代为合法`perf_run`单组合smoke并另对**新签发URL**用FFmpeg解码；不能把native计数当codec验收。

## `udp_rtcp_verify.py`

用途：GB UDP sender/receiver通过proxy验证SR/RR双向实际RTCP报告，观察33秒覆盖原25秒报告间隔。依赖旧Run和未授权控制POST，当前**BLOCKED**。

```bash
python3 bench/udp_rtcp_verify.py --help
```

build-dir/fixture/output必需，client-dir默认build-dir，FFmpeg默认上述路径，port-base26930；没有duration参数。输出rtcp sender_reports/receiver_reports、media、http_flv/result/commands。仅媒体包增长不能替代RTCP SR/RR断言。结束删除自建sender/receiver、关闭proxy和clients/server。替代定向transport/GB CTest加正式GB小smoke；它们并不自动补齐真实SR/RR33秒网络证据，Pi需将本项单列TEST_GAP，不掩盖未执行。

## `gb_signaling_verify.py`

用途：持久device、REGISTER/Digest/Catalog、单上游、多viewer/ticket、停止/重启、Chrome媒体及可选PCMA/RTSP解码。现代码仍启动缺匹配control-token的服务，检查旧`play_id`字段和无TokenFLV/RTSPURL，当前**BLOCKED / NEED_AUTH_MIGRATION**。

```bash
python3 bench/gb_signaling_verify.py --help
```

media/signaling/simulator/output必需；media-file可选Annex-B+AUD，audio-file可选rawPCMA；FFmpeg/browser默认开发机路径，port-base43120。默认生成320×240 H264纯视频，PCMA验收必须提供真实raw文件；没有audio-file不能称音频通过。

输出commands、device/live/ticket与viewer_stats、SDP/Chrome/FFmpeg等原始证据，result预设FAIL、逐check标记。成功原规则需真实browser帧增长，音频分支校验实际sample/RTSP音频。多子服务及browser在finally收口，独立DB保存证据；无确认的cleanup不能单凭script退出0断言资源释放。SIP密码/认证头不可公开。正式替代GB输入的`perf_run`能证明media data-plane，不自动证明REGISTER/Catalog真实SIP E2E；本项待单独授权迁移后验收。

## `gb_simulator_verify.py`

用途：模拟设备控制/媒体/recovery/soak/shutdown/burst回归。与上述GB脚本共享旧返回字段/未授权服务/FLV检查，当前**BLOCKED / NEED_AUTH_MIGRATION**。

```bash
python3 bench/gb_simulator_verify.py --help
```

signaling/simulator/output必需，media默认build/media_server，ffmpeg/browser开发机路径，port-base43420；stage=`correctness|control|media|recovery|soak|shutdown|burst`默认correctness。关键原默认：cycles100、churn100,500,1000、ticket-rounds1000、expiry-batches100,1000、race-rounds100、fleet-counts10..10000、rate-devices1000、control-soak600s、offline-rounds20、live-counts1..500、single-soak1800s、multi-soak1200s、多live100。

**不要在本Goal用这些默认值执行压力或soak。** 若未来迁移后做DEV最小功能，必须显式减少实际所选stage的count；未知stage参数不能猜测。输出stage目录、commands、过程samples、simulatorsummary、result与退出码。预期离线/注入丢包须与工具失败区分，错误不能当server F边界。finally退出/统计只管理本轮services，不全局杀进程。高并发架构与正常音视频fixture验证是不同负载，不能横比历史万路数字。

## `gb_web_ui_verify.py`

用途：真实管理Web交互/设备点播与播放生命周期；不是布局mock或性能工具。signaling/simulator/output必需，media默认build/media_server、port-base43220、ffmpeg/browser默认开发机路径。互斥`--management-only/--playback-only/--rtsp-only`限制范围，默认全套。

```bash
python3 bench/gb_web_ui_verify.py --help
```

管理分支可以不启动media，但脚本仍有旧授权/API与源码相对路径依赖；**本轮网络/媒体分支BLOCKED，管理分支NOT_RUN**。不要把management-only可能通过当全部媒体PASS。输出browser版本、getStats、device/live检查、commands、原始日志/result；finally关闭自启browser/services。SIP/Chrome/真实媒体需分别验证。正式现有Web手动验收可用，但不产生此脚本全量PASS；Pi使用正式设备API和新独立ticket记录实际证据。

## `gb_web_ui_visual_verify.py`

用途：API response fixtures驱动的布局密度、焦点、accessibility、loading/error/reduced-motion截图。**不测SIP、媒体输入、浏览器解码或资源生命周期**。

```bash
python3 bench/gb_web_ui_visual_verify.py --help
# 专属最小参数（有实际signaling/Chrome且端口未占用时才执行）
python3 bench/gb_web_ui_visual_verify.py --signaling "$PERF_SIGNALING_BIN" \
  --browser /usr/bin/google-chrome --port 43623 --output "$PERF_NEW_OUTPUT"
```

signaling/output必需，port43623、browser上述默认，可选`--review-only`。依赖Playwright、真实Chrome和当前signaling/web相对源码检查；bundle内无源码不能假定相同source_sha256证据存在。创建独立signaling DB/listener和browser，fixture拦截API；输出多尺寸PNG、sourcehash、page_errors/result。正常必须all checks=true、无pageerrors；review-only状态OBSERVATION不是PASS。finally关闭自有signaling/browser；本轮仅help VERIFIED、UI运行NOT_RUN。脚本会随运行包的`bench/*.py`复制，但源码、Chrome和Playwright并未因此内置；不能用mock API证明server功能。

## 生命周期入口

`lifecycle_verify.py`完整suite、默认、源替换/churn和当前限制单列在 [lifecycle_verify](lifecycle_verify.md)。其旧接口问题直接影响本页前3工具。没有必要为消除文档BLOCKED而复制整个runner；优先现有授权perf_run/perf_publish和定向CTest，在后续独立迁移时复用其实际解码断言。
