// 实时监控（真实 WHEP）：统一设备列表、GB 通道按需请求、画面与 PeerConnection 一一对应、替换与关闭只释放本画面、
// 布局切换竞态、快速替换、各种离开方式的清理、失败不自动重连、视频与音频实际解码。
// 需要 PUSH_DEVICE（正在推流的推流设备）与在线 GB 设备通道；MEDIA_LOG 可选，用于核对服务端会话。
import { readFileSync } from "node:fs";
import { base, shots, check, clickText, text, api, waitFor, summary } from "./lib.mjs";

const GB = "34020000001320000101";
const CHANNEL = "34020000001310000001";
const PUSH = process.env.PUSH_DEVICE;
const GB_KEY = `gb:${GB}:${CHANNEL}`;
const PUSH_KEY = `push:${PUSH}`;

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

// 页面内注入：记录 PeerConnection、WHEP DELETE、通道列表请求；可延迟 /api/play 与 GB 点播请求。
const instrument = `(() => {
  if (window.__pcs) return;
  window.__pcs = []; window.__deletes = []; window.__channelRequests = []; window.__delay = 0;
  const Original = window.RTCPeerConnection;
  window.RTCPeerConnection = class extends Original { constructor(...args) { super(...args); window.__pcs.push(this); } };
  const fetchOriginal = window.fetch;
  window.fetch = async (url, options = {}) => {
    const path = String(url);
    if (path.endsWith('/channels')) window.__channelRequests.push(path);
    if ((path.includes('/api/play') || path.endsWith('/play')) && window.__delay) await new Promise(resolve => setTimeout(resolve, window.__delay));
    const response = await fetchOriginal(url, options);
    if (options.method === 'DELETE' && path.includes('/whep/')) window.__deletes.push(response.status);
    return response;
  };
})()`;

// 每个连接中 / 播放中的画面持有一个独有的未关闭连接，未关闭连接数等于这些画面数。
const consistency = `(() => {
  const open = window.__pcs.filter(pc => pc.signalingState !== 'closed');
  const tiles = [...document.querySelectorAll('.grid-wrap .tile:not(.is-empty)')].map(tile => {
    const video = tile.querySelector('video');
    const ids = video.srcObject ? video.srcObject.getTracks().map(track => track.id) : [];
    const owner = open.findIndex(pc => pc.getReceivers().some(receiver => ids.includes(receiver.track.id)));
    return { state: tile.className.match(/is-(\\w+)/)[1], owner, frames: video.getVideoPlaybackQuality().totalVideoFrames };
  });
  const live = tiles.filter(tile => ['preparing', 'negotiating', 'streaming'].includes(tile.state));
  const owners = tiles.filter(tile => tile.state === 'streaming').map(tile => tile.owner);
  const ok = open.length === live.length && owners.every(owner => owner >= 0) && new Set(owners).size === owners.length;
  return { ok, open: open.length, tiles: tiles.map(tile => tile.state + (tile.owner >= 0 ? '#' + tile.owner : '') + ':' + tile.frames).join(',') };
})()`;

// 画面实际解码情况：视频帧数，以及 inbound-rtp 音频已接收并解码的样本数（WebRTC 统计）。
const decoding = index => `(async () => {
  const tile = document.querySelectorAll('.grid-wrap .tile')[${index}];
  const video = tile.querySelector('video');
  const ids = video.srcObject ? video.srcObject.getTracks().map(track => track.id) : [];
  const pc = window.__pcs.find(item => item.signalingState !== 'closed' && item.getReceivers().some(receiver => ids.includes(receiver.track.id)));
  let audio = 0;
  if (pc) for (const report of (await pc.getStats()).values()) if (report.type === 'inbound-rtp' && report.kind === 'audio') audio = report.totalSamplesReceived || 0;
  return { frames: video.getVideoPlaybackQuality().totalVideoFrames, width: video.videoWidth, audio, tracks: ids.length };
})()`;

export default async function (b) {
  const q = js => b.eval(js);
  const play = key => q(`document.querySelector('[data-key="play:${key}"]').click()`);
  const expand = () => q(`document.querySelector('[data-key="toggle:${GB}"]').click()`);
  const layout = n => q(clickText(".player-toolbar .segmented button", n === 1 ? "单画面" : "四分屏"));
  const select = i => q(`document.querySelectorAll('.grid-wrap .tile')[${i}].click()`);
  const closeTile = i => q(`document.querySelectorAll('.grid-wrap .tile')[${i}].querySelector('[aria-label="关闭画面"]').click()`);
  const closeAll = () => q(clickText(".page-head .btn", "关闭全部画面"));
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
  const expandAndWait = async () => {
    await expand();
    await waitFor(b, `!!document.querySelector('[data-key="play:${GB_KEY}"]')`, 5000);
  };
  const playGB = async () => {
    await play(GB_KEY);
    await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 1", 20000);
  };
  await stopLive();
  const logStart = process.env.MEDIA_LOG ? readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").length - 1 : 0;
  await b.viewport(1440, 900);
  await b.goto(base + "#devices", 1500);
  await q(instrument);
  await q("location.hash = '#monitor'");
  await b.sleep(3000);
  check("进入实时监控不自动播放", await openPCs() === 0 && (await states()) === "empty,empty,empty,empty");
  const directory = await q(text(".directory"));
  check("统一设备列表显示协议与状态", directory.includes("GB28181") && directory.includes("RTMP") && directory.includes("RTSP") && directory.includes("已停止"));
  check("未展开时不请求 GB 通道", (await q("window.__channelRequests.length")) === 0);

  // 展开 GB 设备才请求通道；收起后不再请求
  await expandAndWait();
  check("展开后显示真实通道", (await q(text(".directory"))).includes("simulator camera"));
  await b.sleep(5500);
  const whileOpen = await q("window.__channelRequests.length");
  check("展开期间随轮询同步该设备通道", whileOpen >= 2, String(whileOpen));
  await expand();
  await b.sleep(5500);
  check("收起后不再请求通道", (await q("window.__channelRequests.length")) === whileOpen);
  await expandAndWait();

  // 搜索：通道、协议；显示范围
  const search = value => q(`(() => { const input = document.querySelector('.resource-pane input[type=search]'); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, ${JSON.stringify(value)}); input.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await search("simulator");
  check("按通道名搜索", (await q(text(".directory"))).includes("simulator camera") && !(await q(text(".directory"))).includes("直播间"));
  await search("rtmp");
  check("按协议搜索", (await q(text(".directory"))).includes("直播间") && !(await q(text(".directory"))).includes("园区东门"));
  await search("");
  const range = value => q(`(() => { const s = document.querySelector('.resource-pane select'); s.value = '${value}'; s.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  await range("ready");
  check("仅在线 / 运行中", !(await q(text(".directory"))).includes("研发楼") && (await q(text(".directory"))).includes("园区东门"));
  await range("all");

  // 单路推流画面：视频与音频解码
  await play(PUSH_KEY);
  check("推流画面开始播放", await waitFor(b, "document.querySelector('.tile.is-streaming video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000));
  await consistent("一路画面对应一个连接", "streaming,empty,empty,empty");
  await b.sleep(1500);
  const pushMedia = await q(decoding(0));
  check("推流画面视频解码（1280 宽）", pushMedia.frames > 20 && pushMedia.width === 1280, JSON.stringify(pushMedia));
  check("推流画面音频（AAC 推流）已解码", pushMedia.tracks === 2 && pushMedia.audio > 0, JSON.stringify(pushMedia));

  // GB 点播画面（播放时按现有流程点播），填入下一个空画面
  check("填入后选中下一个空画面", (await q(text(".player-toolbar"))).includes("画面 2"));
  await play(GB_KEY);
  check("GB 画面开始播放", await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 2", 20000));
  await consistent("两路画面各有独立连接", "streaming,streaming,empty,empty");
  await b.sleep(2000);
  const gbMedia = await q(decoding(1));
  check("GB 画面视频解码（640 宽）", gbMedia.frames > 20 && gbMedia.width === 640, JSON.stringify(gbMedia));
  console.log("INFO GB 通道音频（PCMA 源）:", JSON.stringify(gbMedia));
  await b.shot(`${shots}/monitor-quad-real.png`);

  // 同一视频已在画面中：不重复建立连接
  await play(GB_KEY);
  check("同一视频不重复播放", (await q(text("#toasts"))).includes("已在画面中播放") && await openPCs() === 2);

  // 关闭 GB 画面：只释放本画面，上游点播保留，另一路继续
  const deletes0 = await q("window.__deletes.length");
  const framesBefore = await q("document.querySelectorAll('.tile.is-streaming video')[0].getVideoPlaybackQuality().totalVideoFrames");
  await closeTile(1);
  await b.sleep(800);
  check("关闭画面发送 WHEP DELETE", await q("window.__deletes.length") === deletes0 + 1 && [200, 204, 404].includes(await q("window.__deletes.at(-1)")));
  await consistent("关闭一路后只剩一个连接", "streaming,empty,empty,empty");
  check("关闭画面不停止 GB 上游点播", (await api(`/api/devices/${GB}/channels`)).body.channels[0].live?.state === "streaming");
  check("另一路继续解码", await q("document.querySelector('.tile.is-streaming video').getVideoPlaybackQuality().totalVideoFrames") > framesBefore);

  // 替换：选中正在播放的画面 1，用 GB 通道替换推流画面
  await select(0);
  await play(GB_KEY);
  check("替换画面：关闭原连接并播放新视频", await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 1 && document.querySelector('.tile.is-streaming .name').textContent.includes('simulator')", 15000));
  await consistent("替换后只有一个连接", "streaming,empty,empty,empty");

  // 快速替换：连接中再次替换，只保留最后一个连接
  await q("window.__delay = 1500");
  await select(0);
  await play(PUSH_KEY);
  await b.sleep(100);
  await select(0);
  await play(GB_KEY);
  await b.sleep(3500);
  await consistent("连接中再次替换：无残留连接", "streaming,empty,empty,empty");
  check("最终画面为最后选择的视频", (await q(text(".grid-wrap .tile .name"))).includes("simulator"));
  await q("window.__delay = 0");

  // 竞态：四分屏两路，第二路连接中选中并切单画面，连接完成后正确显示且无幽灵连接
  await closeAll();
  await b.sleep(500);
  await stopLive();
  await b.sleep(500);
  await play(PUSH_KEY);
  await b.sleep(200);
  await q("window.__delay = 1500");
  await play(GB_KEY);
  await b.sleep(100);
  await select(1);
  await layout(1);
  await b.sleep(150);
  check("切单画面瞬间：保留连接中的第二路", (await states()) !== "streaming" && await openPCs() === 1, `${await states()} open=${await openPCs()}`);
  check("连接完成后单画面正确播放", await waitFor(b, "document.querySelector('.grid-wrap .tile.is-streaming video')?.getVideoPlaybackQuality().totalVideoFrames > 5", 20000));
  await consistent("单画面无幽灵连接", "streaming");
  check("单画面是 GB 画面", (await q(text(".grid-wrap .tile .name"))).includes("simulator camera"));
  await b.shot(`${shots}/monitor-single-real.png`);
  await layout(4);
  await b.sleep(300);
  await consistent("切回四分屏仍是同一连接", "streaming,empty,empty,empty");
  await q("window.__delay = 0");

  // 单画面连接中切四分屏
  await closeAll();
  await layout(1);
  await q("window.__delay = 1500");
  await play(PUSH_KEY);
  await b.sleep(100);
  await layout(4);
  check("单画面连接中切四分屏：完成后正常播放", await waitFor(b, "document.querySelectorAll('.tile.is-streaming').length === 1", 15000));
  await consistent("切四分屏后一致", "streaming,empty,empty,empty");

  // 连接中关闭画面：迟到结果不恢复
  await select(1);
  await play(GB_KEY);
  await b.sleep(200);
  await closeTile(1);
  await b.sleep(3000);
  await consistent("连接中关闭：不被迟到结果恢复", "streaming,empty,empty,empty");
  await q("window.__delay = 0");

  // 失败：申请期间推流停止 → 播放失败，不自动重连
  await closeAll();
  await b.sleep(300);
  await q("window.__delay = 1200");
  await play(PUSH_KEY);
  await api(`/api/push-devices/${PUSH}/stop`, { method: "POST" });
  check("媒体流不在时画面显示播放失败", await waitFor(b, "!!document.querySelector('.tile.is-failed')", 8000), await states());
  await q("window.__delay = 0");
  const pcsAtFailure = await q("window.__pcs.length");
  await b.sleep(4000);
  check("失败后不自动重连", await q("window.__pcs.length") === pcsAtFailure && await openPCs() === 0);
  check("失败原因可读", (await q(text(".tile.is-failed"))).includes("媒体流未运行"), await q(text(".tile.is-failed")));
  await consistent("失败画面不持有连接", "failed,empty,empty,empty");
  await b.shot(`${shots}/monitor-failed.png`);
  await q(clickText(".tile .btn", "关闭"));

  // 离开方式：侧边栏 / 后退 / 前进 / 改 hash / 画面详情链接 / pagehide
  await playGB();
  const d1 = await q("window.__deletes.length");
  await q(clickText(".nav-item", "设备管理"));
  await b.sleep(800);
  check("侧边栏离开：释放全部连接并 DELETE", await openPCs() === 0 && await q("window.__deletes.length") === d1 + 1);
  await q("history.back()");
  await b.sleep(1200);
  check("浏览器后退回到实时监控：不恢复播放", (await q("location.hash")) === "#monitor" && await openPCs() === 0 && (await states()) === "empty,empty,empty,empty");
  await expandAndWait();
  await playGB();
  await q("history.back()");
  await b.sleep(800);
  check("浏览器后退离开：释放全部连接", (await q("location.hash")) !== "#monitor" && await openPCs() === 0);
  await q("history.forward()");
  await b.sleep(1200);
  check("浏览器前进回来：不恢复播放", (await q("location.hash")) === "#monitor" && await openPCs() === 0);
  await expandAndWait();
  await playGB();
  await q("location.hash = '#devices'");
  await b.sleep(800);
  check("修改 hash 离开：释放全部连接", await openPCs() === 0);
  await q("location.hash = '#monitor'");
  await b.sleep(1200);
  await expandAndWait();
  await playGB();
  await q(`document.querySelector('.tile [aria-label="设备详情"]').click()`);
  await b.sleep(800);
  check("从画面打开设备详情：释放连接并进入详情", (await q("location.hash")).startsWith("#devices/gb/") && await openPCs() === 0);
  check("GB 上游点播仍在（只释放观看者）", (await api(`/api/devices/${GB}/channels`)).body.channels[0].live?.state === "streaming");
  await q("location.hash = '#monitor'");
  await b.sleep(1200);
  await expandAndWait();
  await playGB();
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
