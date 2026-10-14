// 详情本页 WHEP：显式播放、真实双轨进展、GB 按需点播、关闭与迟到请求清理。
// 五协议共用一次性授权仍由真实 ffprobe 连接验证，不展示签发地址面板。
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { base, shots, check, api, waitFor, summary } from "./lib.mjs";
const GB = "34020000001320000101";
const CHANNEL = "34020000001310000001";
const PUSH = process.env.PUSH_DEVICE;
function probe(url) {
  try {
    const transport = url.startsWith("rtsp://") ? ["-rtsp_transport", "tcp"] : [];
    return execFileSync("ffprobe", ["-v", "error", ...transport, "-rw_timeout", "4000000", "-select_streams", "v",
      "-show_entries", "stream=codec_name", "-of", "csv=p=0", url], { timeout: 12000, stdio: ["ignore", "pipe", "pipe"] }).toString().includes("h264");
  } catch { return false; }
}
export default async function (b) {
  const q = js => b.eval(js);
  const injected = await b.beforeLoad(`{
    window.__pcs = []; window.__deletes = []; window.__playRequests = 0; window.__delay = 0;
    const Peer = window.RTCPeerConnection;
    window.RTCPeerConnection = class extends Peer { constructor(...args) { super(...args); window.__pcs.push(this); } };
    const original = window.fetch;
    window.fetch = async (url, options = {}) => {
      if (String(url).includes('/api/play') || String(url).endsWith('/play')) {
        window.__playRequests += 1;
        if (window.__delay) await new Promise(resolve => setTimeout(resolve, window.__delay));
      }
      const response = await original(url, options);
      if (options.method === 'DELETE' && String(url).includes('/whep/')) window.__deletes.push(response.status);
      return response;
    };
  }`);
  const open = () => q("window.__pcs.filter(pc => pc.signalingState !== 'closed').length");
  const frames = () => q("document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames || 0");
  const play = () => q("document.querySelector('[data-key=detail-play]').click()");
  const close = () => q("document.querySelector('.detail-player [aria-label=关闭画面]').click()");
  const liveOf = async () => (await api(`/api/devices/${GB}/channels`)).body.channels.find(item => item.channel_id === CHANNEL)?.live;
  const logStart = process.env.MEDIA_LOG ? readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").length - 1 : 0;
  try {
    const oldLive = await liveOf();
    if (oldLive) await api(`/api/lives/${oldLive.live_id}`, { method: "DELETE" });
    await b.viewport(1440, 900);
    await b.goto(base + `#devices/push/${PUSH}`, 2000);
    check("打开详情不自动签发或播放", await open() === 0 && await q("window.__playRequests") === 0);
    check("详情不展示五地址或倒计时", await q("!document.querySelector('.urls-panel') && !document.querySelector('[data-countdown]')"));
    check("详情提供本页播放", await q("!document.querySelector('[data-key=detail-play]').disabled"));
    // API 独立授权不消费本页播放器自己的授权。
    const ticket = (await api("/api/play", { method: "POST", body: { source: { push_device_id: PUSH } } })).body;
    const keys = ["rtsp_url", "rtmp_url", "flv_url", "hls_url", "whep_url"];
    const token = url => url.match(/\/([0-9a-f]{64})(?:\.flv|\/index\.m3u8)?$/)?.[1];
    check("服务端五协议仍共用一次性授权", keys.every(key => ticket[key]) && new Set(keys.map(key => token(ticket[key]))).size === 1 && Boolean(token(ticket.rtsp_url)));
    check("独立 HTTP-FLV 授权实际可播放", probe(ticket.flv_url));
    check("同组 RTSP 已消费授权被拒绝", !probe(ticket.rtsp_url));
    const fresh = (await api("/api/play", { method: "POST", body: { source: { push_device_id: PUSH } } })).body;
    check("独立新授权 RTSP 实际可播放", token(fresh.rtsp_url) !== token(ticket.rtsp_url) && probe(fresh.rtsp_url));
    await play();
    check("本页 WHEP 真正解码且不跳监控", await waitFor(b, "document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000)
      && await q("location.hash") === `#devices/push/${PUSH}` && await open() === 1);
    const samples = await q(`(async () => { let samples = 0; for (const pc of window.__pcs.filter(pc => pc.signalingState !== 'closed'))
      for (const r of (await pc.getStats()).values()) if (r.type === 'inbound-rtp' && r.kind === 'audio') samples += r.totalSamplesReceived || 0; return samples; })()`);
    check("本页双轨与音频样本进展", samples > 0 && await q("document.querySelector('.detail-player video').srcObject.getTracks().length") === 2);
    const before = await frames();
    await b.sleep(1000);
    check("轮询不重建或打断播放器", await frames() > before && await open() === 1);
    await b.shot(`${shots}/detail-push-whep.png`);
    await close();
    check("关闭本页画面发送 DELETE 且关闭连接", await waitFor(b, "window.__pcs.every(pc => pc.signalingState === 'closed') && window.__deletes.length === 1", 5000)
      && [200, 204, 404].includes(await q("window.__deletes[0]")));
    check("关闭画面不停止上游推流", (await api(`/api/push-devices/${PUSH}`)).body.state === "publishing");
    await q("window.__delay = 1800");
    await play();
    await b.sleep(100);
    await close();
    await b.sleep(2200);
    check("连接中关闭后迟到请求不恢复画面", await open() === 0 && await q("document.querySelectorAll('.detail-player video').length") === 0);
    await q("window.__delay = 0");
    await play();
    await b.sleep(100);
    await q(`location.hash = '#devices/gb/${GB}'`);
    await b.sleep(2600);
    check("切设备释放旧 viewer 且没有迟到画面", await open() === 0 && await q("document.querySelectorAll('.detail-player video').length") === 0);
    check("GB 打开详情不点播", !(await liveOf()));
    await play();
    check("GB 点击本页播放完成点播和真实解码", await waitFor(b, "document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 20000)
      && (await liveOf())?.state === "streaming" && await open() === 1);
    await b.shot(`${shots}/detail-gb-whep.png`);
    await close();
    check("关闭 GB 画面保留共享上游", await open() === 0 && (await liveOf())?.state === "streaming");
    await play();
    check("同一上游再次播放使用新 viewer", await waitFor(b, "document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames > 5", 15000));
    const live = await liveOf();
    await api(`/api/lives/${live.live_id}`, { method: "DELETE" });
    check("媒体结束后显示 ended 并释放连接", await waitFor(b, "!!document.querySelector('.detail-player .tile.is-ended') && window.__pcs.every(pc => pc.signalingState === 'closed')", 10000));
    const count = await q("window.__pcs.length");
    await b.sleep(3000);
    check("结束后不自动重连", await q("window.__pcs.length") === count && !(await liveOf()));
    await q("location.hash = '#devices'");
    await b.sleep(700);
    check("离开详情回收所有播放器", await open() === 0 && await q("document.querySelectorAll('video').length") === 0);
    if (process.env.MEDIA_LOG) {
      const lines = readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").slice(logStart);
      const owned = new Set(lines.map(line => line.match(/whep session created (\w+)/)?.[1]).filter(Boolean));
      for (const line of lines) { const id = line.match(/whep session shutdown (\w+)/)?.[1]; if (id) owned.delete(id); }
      check("服务端本轮 WHEP 会话全部关闭", owned.size === 0, String(owned.size));
    }
  } finally {
    await q("location.hash = '#devices'");
    await b.sleep(400);
    const live = await liveOf();
    if (live) await api(`/api/lives/${live.live_id}`, { method: "DELETE" });
    await b.removeBeforeLoad(injected);
  }
  summary();
}
