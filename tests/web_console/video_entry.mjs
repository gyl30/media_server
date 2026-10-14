// 从设备详情显式播放：留在本页，真实解码；窄屏、pagehide、离开与返回均不留下 viewer。
import { base, shots, check, waitFor, summary } from "./lib.mjs";
const PUSH = process.env.PUSH_DEVICE;
export default async function (b) {
  const q = js => b.eval(js);
  const injected = await b.beforeLoad(`{ window.__pcs = []; const Peer = window.RTCPeerConnection;
    window.RTCPeerConnection = class extends Peer { constructor(...args) { super(...args); window.__pcs.push(this); } }; }`);
  try {
    await b.viewport(1440, 900);
    await b.goto(base + `#devices/push/${PUSH}`, 2200);
    check("打开详情不自动播放", await q("window.__pcs.length") === 0);
    check("详情提供本页播放", await q("!document.querySelector('[data-key=detail-play]').disabled"));
    await q("document.querySelector('[data-key=detail-play]').click()");
    check("详情本页播放这一路且不跳实时监控", await waitFor(b, "document.querySelector('.detail-player video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000)
      && await q("location.hash") === `#devices/push/${PUSH}`);
    check("只有一个独立画面与连接", await q("document.querySelectorAll('.detail-player .tile').length === 1 && window.__pcs.filter(pc => pc.signalingState !== 'closed').length === 1"));
    await b.shot(`${shots}/detail-single-real.png`);
    await b.viewport(390, 844);
    await b.sleep(800);
    check("390 详情播放无横向溢出", await q("document.documentElement.scrollWidth <= innerWidth"));
    await b.shot(`${shots}/390-detail-video-real.png`, true);
    await q("window.dispatchEvent(new Event('pagehide'))");
    check("pagehide 关闭本页 viewer", await waitFor(b, "window.__pcs.every(pc => pc.signalingState === 'closed')", 4000));
    await q("location.hash = '#devices'");
    await b.sleep(500);
    check("离开详情不在后台播放", await q("document.querySelectorAll('video').length === 0 && window.__pcs.every(pc => pc.signalingState === 'closed')"));
    await q("history.back()");
    await b.sleep(800);
    check("返回详情不自动恢复", await q("!document.querySelector('.detail-player video') && window.__pcs.every(pc => pc.signalingState === 'closed')"));
  } finally {
    await q("location.hash = '#devices'");
    await b.sleep(400);
    await b.removeBeforeLoad(injected);
  }
  summary();
}
