// 视频中心（真实 WHEP）：画面与 PeerConnection 一一对应、关闭只释放本画面、布局切换竞态、
// 各种离开方式的清理、失败不自动重连。需要 PUSH_DEVICE（正在推流）与在线的 GB 设备通道。
import { readFileSync } from "node:fs";
import { base, shots, check, clickText, text, api, waitFor, summary } from "./lib.mjs";

// 媒体服务器日志（MEDIA_LOG）中本次测试期间创建、但没有 shutdown 的 WHEP 会话。
function leakedSessions(fromLine) {
  if (!process.env.MEDIA_LOG) return null;
  const lines = readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").slice(fromLine);
  const started = new Set(lines.map(line => line.match(/whep session created (\w+)/)?.[1]).filter(Boolean));
  for (const line of lines) {
    const closed = line.match(/whep session shutdown (\w+)/)?.[1];
    if (closed) started.delete(closed);
  }
  return [...started];
}

const GB = "34020000001320000101";
const CHANNEL = "34020000001310000001";
const PUSH = process.env.PUSH_DEVICE;

// 页面内注入：记录 PeerConnection、WHEP 资源 DELETE；可延迟 /api/play。
const instrument = `(() => {
  if (window.__pcs) return;
  window.__pcs = []; window.__deletes = []; window.__delay = 0;
  const Original = window.RTCPeerConnection;
  window.RTCPeerConnection = class extends Original { constructor(...args) { super(...args); window.__pcs.push(this); } };
  const fetchOriginal = window.fetch;
  window.fetch = async (url, options = {}) => {
    if (String(url).includes('/api/play') && window.__delay) await new Promise(resolve => setTimeout(resolve, window.__delay));
    const response = await fetchOriginal(url, options);
    if (options.method === 'DELETE' && String(url).includes('/whep/')) window.__deletes.push(response.status);
    return response;
  };
})()`;

// 画面与连接的对应关系：每个连接中 / 播放中的画面持有一个独有的未关闭连接，未关闭连接数等于这些画面数。
const consistency = `(() => {
  const open = window.__pcs.filter(pc => pc.signalingState !== 'closed');
  const tiles = [...document.querySelectorAll('.grid-wrap .tile:not(.is-empty)')].map(tile => {
    const video = tile.querySelector('video');
    const ids = video.srcObject ? video.srcObject.getTracks().map(track => track.id) : [];
    const owner = open.findIndex(pc => pc.getReceivers().some(receiver => ids.includes(receiver.track.id)));
    return { state: tile.className.match(/is-(\\w+)/)[1], owner, frames: video.getVideoPlaybackQuality().totalVideoFrames, label: tile.querySelector('.name')?.textContent };
  });
  const live = tiles.filter(tile => ['preparing', 'negotiating', 'streaming'].includes(tile.state));
  const owners = tiles.filter(tile => tile.state === 'streaming').map(tile => tile.owner);
  const ok = open.length === live.length && owners.every(owner => owner >= 0) && new Set(owners).size === owners.length;
  return { ok, open: open.length, tiles: tiles.map(tile => tile.state + (tile.owner >= 0 ? '#' + tile.owner : '') + ':' + tile.frames).join(',') };
})()`;

export default async function (b) {
  const q = js => b.eval(js);
  const pick = name => q(clickText(".tree-node", name));
  const layout = n => q(clickText(".stage .segmented button", n === 1 ? "单画面" : "四分屏"));
  const states = () => q("[...document.querySelectorAll('.grid-wrap .tile')].map(t => t.className.match(/is-(\\w+)/)[1]).join(',')");
  const openPCs = () => q("window.__pcs.filter(pc => pc.signalingState !== 'closed').length");
  const consistent = async (label, expect) => {
    const r = await q(consistency);
    check(label, r.ok && (!expect || (await states()) === expect), `${await states()} | open=${r.open} ${r.tiles}`);
  };
  const stopLive = async () => {
    const live = (await api(`/api/devices/${GB}/channels`)).body.channels.find(item => item.channel_id === CHANNEL)?.live;
    if (live) await api(`/api/lives/${live.live_id}`, { method: "DELETE" });
  };
  await stopLive();
  const logStart = process.env.MEDIA_LOG ? readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").length - 1 : 0;
  await b.viewport(1440, 900);
  await b.goto(base + "#devices", 2000);
  await q(instrument);
  await q("location.hash = '#video'");
  await b.sleep(1500);
  check("进入视频中心不自动播放", await openPCs() === 0 && (await states()) === "empty,empty,empty,empty");

  // 单路：推流设备
  await pick("直播间 OBS");
  check("推流画面开始播放", await waitFor(b, "document.querySelector('.tile.is-streaming video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000));
  await consistent("一路画面对应一个连接", "streaming,empty,empty,empty");

  // 两路：GB 点播（视频中心播放时按现有流程点播）
  await pick("simulator camera");
  check("GB 画面开始播放", await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 2", 20000));
  await consistent("两路画面各有独立连接", "streaming,streaming,empty,empty");
  await b.sleep(1500);
  await consistent("两路持续解码");
  await b.shot(`${shots}/video-quad-real.png`);

  // 关闭 GB 画面：只释放本画面，上游点播保留，另一路继续
  const deletes = await q("window.__deletes.length");
  const framesBefore = await q("document.querySelectorAll('.tile.is-streaming video')[0].getVideoPlaybackQuality().totalVideoFrames");
  await q(`document.querySelectorAll('.grid-wrap .tile')[1].querySelector('[aria-label="关闭画面"]').click()`);
  await b.sleep(800);
  // 关闭时 DELETE 与 PeerConnection 关闭同时发出；服务端可能先因 DTLS 关闭释放会话，DELETE 得到 404（whep.js 视为已释放）。
  check("关闭画面发送 WHEP DELETE", await q("window.__deletes.length") === deletes + 1 && [200, 204, 404].includes(await q("window.__deletes.at(-1)")), JSON.stringify(await q("window.__deletes")));
  await consistent("关闭一路后只剩一个连接", "streaming,empty,empty,empty");
  check("关闭画面不停止 GB 上游点播", (await api(`/api/devices/${GB}/channels`)).body.channels[0].live?.state === "streaming");
  check("另一路继续解码", await q("document.querySelector('.tile.is-streaming video').getVideoPlaybackQuality().totalVideoFrames") > framesBefore);

  // 竞态：四分屏两路，第二路连接中选中并切单画面，连接完成后正确显示且无幽灵连接
  await q(clickText(".stage .btn", "全部关闭"));
  await b.sleep(500);
  await stopLive();
  await b.sleep(500);
  await pick("直播间 OBS");
  await b.sleep(200);
  await q("window.__delay = 1500");
  await pick("simulator camera");
  await b.sleep(100);
  await q("document.querySelectorAll('.grid-wrap .tile')[1].click()");
  await layout(1);
  await b.sleep(150);
  check("切单画面瞬间：保留连接中的第二路", (await states()) !== "streaming" && await openPCs() === 1, `${await states()} open=${await openPCs()}`);
  check("连接完成后单画面正确播放", await waitFor(b, "document.querySelector('.grid-wrap .tile.is-streaming video')?.getVideoPlaybackQuality().totalVideoFrames > 5", 20000));
  await consistent("单画面无幽灵连接", "streaming");
  check("单画面是 GB 画面", (await q(text(".grid-wrap .tile .name"))).includes("simulator camera"));
  await layout(4);
  await b.sleep(300);
  await consistent("切回四分屏仍是同一连接", "streaming,empty,empty,empty");
  await q("window.__delay = 0");

  // 单画面连接中切四分屏
  await q(clickText(".stage .btn", "全部关闭"));
  await layout(1);
  await q("window.__delay = 1500");
  await pick("直播间 OBS");
  await b.sleep(100);
  await layout(4);
  check("单画面连接中切四分屏：完成后正常播放", await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 1", 15000));
  await consistent("切四分屏后一致", "streaming,empty,empty,empty");
  await q("window.__delay = 0");

  // 连接中关闭画面：迟到结果不重建连接
  await q("window.__delay = 1500");
  await pick("simulator camera");
  await b.sleep(200);
  await q(`document.querySelectorAll('.grid-wrap .tile')[1].querySelector('[aria-label="关闭画面"]').click()`);
  await b.sleep(3000);
  await consistent("连接中关闭：不被迟到结果恢复", "streaming,empty,empty,empty");
  await q("window.__delay = 0");

  // 失败：申请期间推流停止 → 播放失败，不自动重连；手动重试
  await q(clickText(".stage .btn", "全部关闭"));
  await b.sleep(300);
  await q("window.__delay = 1200");
  await pick("直播间 OBS");
  await api(`/api/push-devices/${PUSH}/stop`, { method: "POST" });
  check("媒体流不在时画面显示播放失败", await waitFor(b, "!!document.querySelector('.tile.is-failed')", 8000), await states());
  await q("window.__delay = 0");
  const pcsAtFailure = await q("window.__pcs.length");
  await b.sleep(4000);
  check("失败后不自动重连", await q("window.__pcs.length") === pcsAtFailure && await openPCs() === 0);
  await consistent("失败画面不持有连接", "failed,empty,empty,empty");
  await b.shot(`${shots}/video-failed.png`);
  await q(clickText(".tile .btn", "关闭"));

  // 离开方式：侧边栏 / 后退 / 前进 / 改 hash / 画面详情链接 / pagehide
  const playTwo = async () => {
    await pick("simulator camera");
    await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 1", 20000);
  };
  await playTwo();
  const d1 = await q("window.__deletes.length");
  await q(clickText(".nav-item", "设备管理"));
  await b.sleep(800);
  check("侧边栏离开：释放全部连接并 DELETE", await openPCs() === 0 && await q("window.__deletes.length") === d1 + 1);
  await q("history.back()");
  await b.sleep(800);
  check("浏览器后退回到视频中心：不恢复播放", (await q("location.hash")) === "#video" && await openPCs() === 0 && (await states()) === "empty,empty,empty,empty");
  await playTwo();
  await q("history.back()");
  await b.sleep(800);
  check("浏览器后退离开：释放全部连接", (await q("location.hash")) !== "#video" && await openPCs() === 0);
  await q("history.forward()");
  await b.sleep(800);
  check("浏览器前进回来：不恢复播放", (await q("location.hash")) === "#video" && await openPCs() === 0);
  await playTwo();
  await q("location.hash = '#devices'");
  await b.sleep(800);
  check("修改 hash 离开：释放全部连接", await openPCs() === 0);
  await q("location.hash = '#video'");
  await b.sleep(800);
  await playTwo();
  await q(`document.querySelector('.tile [aria-label="设备详情"]').click()`);
  await b.sleep(800);
  check("从画面打开设备详情：释放连接并进入详情", (await q("location.hash")).startsWith("#devices/gb/") && await openPCs() === 0);
  check("GB 上游点播仍在（只释放观看者）", (await api(`/api/devices/${GB}/channels`)).body.channels[0].live?.state === "streaming");
  await q("location.hash = '#video'");
  await b.sleep(800);
  await playTwo();
  const d2 = await q("window.__deletes.length");
  await q("window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: false }))");
  await b.sleep(800);
  check("pagehide：关闭全部连接并发送 keepalive DELETE", await openPCs() === 0 && await q("window.__deletes.length") === d2 + 1);
  check("所有 DELETE 都得到 2xx 或 404", (await q("window.__deletes")).every(status => [200, 204, 404].includes(status)), JSON.stringify(await q("window.__deletes")));
  await b.sleep(2000);
  const leaked = leakedSessions(logStart);
  check(leaked === null ? "服务端会话核对（未提供 MEDIA_LOG，跳过）" : "服务端：本次创建的 WHEP 会话全部已关闭", leaked === null || leaked.length === 0, JSON.stringify(leaked));
  summary();
}
