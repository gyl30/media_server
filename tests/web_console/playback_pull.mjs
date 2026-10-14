// RTSP 主动拉流在详情本页播放；独立未消费授权真实等到过期，已建立 viewer 持续解码。
import { base, shots, check, api, waitFor, summary } from "./lib.mjs";
const PUSH = process.env.PUSH_DEVICE;
export default async function (b) {
  const q = js => b.eval(js);
  let source;
  const injected = await b.beforeLoad(`{ window.__pcs = []; const Peer = window.RTCPeerConnection;
    window.RTCPeerConnection = class extends Peer { constructor(...args) { super(...args); window.__pcs.push(this); } }; }`);
  try {
    for (const old of (await api("/api/sources")).body.sources.filter(item => item.stream_name === "拉流测试 源")) await api(`/api/sources/${old.source_id}`, { method: "DELETE" });
    const ticket = (await api("/api/play", { method: "POST", body: { source: { push_device_id: PUSH } } })).body;
    source = (await api("/api/sources", { method: "POST", body: { stream_name: "拉流测试 源", url: ticket.rtsp_url } })).body;
    await b.viewport(1440, 900);
    await b.goto(base + `#devices/rtsp/${source.source_id}`, 1800);
    check("添加即自动拉流", source.desired_state === "running" && Boolean(source.session));
    check("媒体就绪后播放按钮可用，但不自动播放", await waitFor(b, "document.querySelector('[data-key=detail-play]')?.disabled === false", 15000)
      && await q("window.__pcs.length") === 0);
    const session = (await api("/api/sources")).body.sources.find(item => item.source_id === source.source_id).session;
    const unused = (await api("/api/play", { method: "POST", body: { source: { source_id: source.source_id } } })).body;
    check("播放授权属于拉流会话", session?.state === "created" && unused.stream_id === session.stream_id);
    await q("document.querySelector('[data-key=detail-play]').click()");
    check("拉流详情本页真实解码", await waitFor(b, "document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000));
    const before = await q("document.querySelector('.detail-player video').getVideoPlaybackQuality().totalVideoFrames");
    await b.shot(`${shots}/detail-rtsp-whep.png`);
    await b.sleep(Math.max(0, Date.parse(unused.expires_at) - Date.now() + 1200));
    const rejected = await fetch(unused.flv_url);
    await rejected.arrayBuffer();
    check("独立未消费授权到期后真实拒绝", rejected.status === 403);
    check("授权过期不打断已建立播放", await q("document.querySelector('.detail-player video').getVideoPlaybackQuality().totalVideoFrames") > before + 100
      && await q("window.__pcs.filter(pc => pc.signalingState !== 'closed').length") === 1);
    await api(`/api/sources/${source.source_id}/stop`, { method: "POST" });
    check("停止拉流结束本页画面且关闭连接", await waitFor(b, "!!document.querySelector('.detail-player .tile.is-ended') && window.__pcs.every(pc => pc.signalingState === 'closed')", 10000));
  } finally {
    await q("location.hash = '#devices'");
    await b.sleep(400);
    if (source) await api(`/api/sources/${source.source_id}`, { method: "DELETE" });
    await b.removeBeforeLoad(injected);
  }
  summary();
}
