// 七种真实输入 → 详情页显式 WHEP 播放。只观察原生 Peer/getStats/fetch，不替换 SDP、响应或媒体统计。
// root 串行调度：WEB_CONSOLE_FIXTURES=real-env/fixtures.json，其他变量沿用 CDP/lib。
import { readFileSync, writeFileSync } from "node:fs";
import { base, shots, check, api, waitFor, summary } from "./lib.mjs";

const fixturePath = process.env.WEB_CONSOLE_FIXTURES;
if (!fixturePath) throw new Error("WEB_CONSOLE_FIXTURES is required");
const fixtures = JSON.parse(readFileSync(fixturePath, "utf8"));
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
for (const protocol of ["rtsp", "rtmp", "whep"]) {
  if (!uuid.test(fixtures.sources?.[protocol] || "")) throw new Error(`missing ${protocol} pull fixture UUID`);
}
for (const protocol of ["rtmp", "rtsp", "whip"]) {
  if (!uuid.test(fixtures.pushes?.[protocol] || "")) throw new Error(`missing ${protocol} publish fixture UUID`);
}
if (!/^\d{20}$/.test(fixtures.gb?.device_id || "") || !/^\d{20}$/.test(fixtures.gb?.channel_id || "")) {
  throw new Error("missing GB device/channel fixture IDs");
}
const cases = [
  ...["rtsp", "rtmp", "whep"].map(protocol => ({
    label: `${protocol.toUpperCase()} pull`, kind: "rtsp", id: fixtures.sources[protocol], protocol, width: 1280, height: 720,
  })),
  ...["rtmp", "rtsp", "whip"].map(protocol => ({
    label: `${protocol.toUpperCase()} publish`, kind: "push", id: fixtures.pushes[protocol], protocol, width: 1280, height: 720,
  })),
  { label: "GB28181", kind: "gb", id: fixtures.gb.device_id, channel: fixtures.gb.channel_id, width: 640, height: 360 },
];
if (new Set(cases.map(input => `${input.kind}:${input.id}`)).size !== cases.length) throw new Error("seven distinct input fixtures are required");

const observe = `{
  window.__multiInput = { peers: [], created: [], deleted: [], authorizations: 0, requests: [] };
  const OriginalPeer = window.RTCPeerConnection;
  window.RTCPeerConnection = class extends OriginalPeer {
    constructor(...args) { super(...args); window.__multiInput.peers.push(this); }
  };
  const originalFetch = window.fetch;
  window.fetch = async function(input, options) {
    const response = await originalFetch.call(this, input, options);
    const address = new URL(input instanceof Request ? input.url : String(input), location.href);
    const method = String(options?.method || (input instanceof Request ? input.method : "GET")).toUpperCase();
    const state = window.__multiInput;
    if (method === "POST" && (address.pathname === "/api/play" || new RegExp("^/api/devices/[^/]+/channels/[^/]+/play$").test(address.pathname))) {
      state.authorizations += 1;
      let source;
      try { source = JSON.parse(options?.body || "{}").source; } catch {}
      state.requests.push({ path: address.pathname, source });
    }
    if (method === "POST" && address.pathname.startsWith("/play/whep/") && response.status === 201) {
      const resource = response.headers.get("Location");
      state.created.push({ resource: resource ? new URL(resource, address).href : "", status: response.status });
    }
    if (method === "DELETE" && address.pathname.includes("/whep/")) {
      state.deleted.push({ resource: address.href, status: response.status });
    }
    return response;
  };
}`;

// 所有数值都直接读取当前 DOM 轨道所属原生 PeerConnection 的原生 RTCStatsReport。
const snapshot = `(async () => {
  const video = document.querySelector(".detail-player video");
  const tracks = video?.srcObject?.getTracks() || [];
  const ids = tracks.map(track => track.id);
  const peers = window.__multiInput.peers;
  const owners = peers.filter(peer => peer.signalingState !== "closed" &&
    peer.getReceivers().some(receiver => receiver.track && ids.includes(receiver.track.id)));
  if (!video || owners.length !== 1) return { owners: owners.length, tracks: tracks.length };
  const peer = owners[0];
  const reports = [...(await peer.getStats()).values()].filter(report => report.type === "inbound-rtp" && !report.isRemote);
  const videos = reports.filter(report => (report.kind || report.mediaType) === "video");
  const audios = reports.filter(report => (report.kind || report.mediaType) === "audio");
  const v = videos[0], a = audios[0];
  return {
    peer: peers.indexOf(peer), owners: owners.length, connection: peer.connectionState,
    tracks: tracks.length, videoTracks: tracks.filter(track => track.kind === "video" && track.readyState === "live").length,
    audioTracks: tracks.filter(track => track.kind === "audio" && track.readyState === "live").length,
    videoReports: videos.length, audioReports: audios.length, videoReport: v?.id, audioReport: a?.id,
    width: video.videoWidth, height: video.videoHeight,
    elementFrames: video.getVideoPlaybackQuality().totalVideoFrames,
    framesDecoded: v?.framesDecoded, videoBytes: v?.bytesReceived,
    audioBytes: a?.bytesReceived, samples: a?.totalSamplesReceived,
    duration: a?.totalSamplesDuration, energy: a?.totalAudioEnergy,
  };
})()`;

function ready(value) {
  return value?.owners === 1 && value.connection === "connected" && value.tracks === 2 &&
    value.videoTracks === 1 && value.audioTracks === 1 && value.videoReports === 1 && value.audioReports === 1 &&
    [value.framesDecoded, value.elementFrames, value.videoBytes, value.audioBytes, value.samples, value.duration, value.energy]
      .every(number => Number.isFinite(number) && number > 0);
}

function unclosedServerSessions(fromLine) {
  if (!process.env.MEDIA_LOG) return [];
  const lines = readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").slice(fromLine);
  const pending = new Set();
  for (const line of lines) {
    const created = line.match(/whep session created (\w+)/)?.[1];
    const closed = line.match(/whep session shutdown (\w+)/)?.[1];
    if (created) pending.add(created);
    if (closed) pending.delete(closed);
  }
  return [...pending];
}

export default async function (b) {
  const q = expression => b.eval(expression);
  const results = [];
  let completed = 0;
  const assert = (label, condition, detail = "") => {
    check(label, condition, detail);
    if (!condition) throw new Error(label);
  };
  const open = () => q("window.__multiInput.peers.filter(peer => peer.signalingState !== 'closed').length");
  const channels = async () => {
    const response = await api(`/api/devices/${fixtures.gb.device_id}/channels`);
    if (response.status !== 200) throw new Error("GB fixture channel API unavailable");
    return response.body.channels;
  };
  const logStart = process.env.MEDIA_LOG ? readFileSync(process.env.MEDIA_LOG, "utf8").split("\n").length - 1 : 0;
  const injected = await b.beforeLoad(observe);
  try {
    await b.viewport(1440, 900);
    await b.goto(base + "#devices", 1500);
    for (const input of cases) {
      if (input.kind === "rtsp") {
        const response = await api("/api/sources");
        const source = response.body?.sources?.find(value => value.source_id === input.id);
        const scheme = { rtsp: "rtsp:", rtmp: "rtmp:", whep: "http:" }[input.protocol];
        assert(`${input.label}: 实际来源配置协议匹配 fixture`, response.status === 200 && Boolean(source) && new URL(source.url).protocol === scheme);
      } else if (input.kind === "push") {
        const response = await api(`/api/push-devices/${input.id}`);
        assert(`${input.label}: 实际发布设备协议匹配 fixture`, response.status === 200 && response.body?.protocol === input.protocol);
      }
      const before = await q("({ peers: window.__multiInput.peers.length, created: window.__multiInput.created.length, deleted: window.__multiInput.deleted.length, authorizations: window.__multiInput.authorizations })");
      let oldLive;
      if (input.kind === "gb") oldLive = (await channels()).find(channel => channel.channel_id === input.channel)?.live?.live_id;
      await q(`location.hash = ${JSON.stringify(`#devices/${input.kind}/${input.id}`)}`);
      assert(`${input.label}: 详情播放按钮等待真实媒体就绪`, await waitFor(b,
        `location.hash === ${JSON.stringify(`#devices/${input.kind}/${input.id}`)} && document.querySelector('[data-key="detail-play"]')?.disabled === false`, 30000));
      if (input.kind === "gb") {
        assert(`${input.label}: 选择指定真实通道`, await waitFor(b,
          `!!document.querySelector('[data-key="pick:${input.channel}"]')`, 10000));
        await q(`document.querySelector('[data-key="pick:${input.channel}"]').click()`);
        const live = (await channels()).find(channel => channel.channel_id === input.channel)?.live?.live_id;
        assert(`${input.label}: 打开详情不新建点播`, live === oldLive);
      }
      assert(`${input.label}: 打开详情不自动授权或建 viewer`, await open() === 0 && await q(
        `window.__multiInput.peers.length === ${before.peers} && window.__multiInput.created.length === ${before.created} && window.__multiInput.authorizations === ${before.authorizations}`));
      await q("document.querySelector('[data-key=detail-play]').click()");
      assert(`${input.label}: 静音首播真实视频及音频样本就绪`, await waitFor(b,
        `(${snapshot}).then(value => value.connection === 'connected' && value.framesDecoded > 10 && value.samples > 0)`, 35000));
      // 默认静音不代表音频已实际播放；使用真实鼠标操作启用声音后再要求非零音频能量。
      const sound = await q(`(() => { const button = document.querySelector('.detail-player [aria-label="打开声音"]');
        const rect = button?.getBoundingClientRect(); return rect && { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 }; })()`);
      assert(`${input.label}: 页面提供实际声音开关`, Boolean(sound));
      await b.click(sound.x, sound.y);
      assert(`${input.label}: 用户操作已解除静音`, await q("document.querySelector('.detail-player video').muted === false"));
      assert(`${input.label}: 显式播放建立真实 WHEP 双轨`, await waitFor(b,
        `(${snapshot}).then(value => value.owners === 1 && value.connection === 'connected' && value.tracks === 2 && value.framesDecoded > 10 && value.samples > 0 && value.duration > 0 && value.energy > 0)`, 35000), JSON.stringify(await q(snapshot)));
      const requests = await q(`window.__multiInput.requests.slice(${before.authorizations})`);
      assert(`${input.label}: 真实授权请求只指向本输入`, requests.length > 0 && requests.every(request => {
        if (input.kind === "gb") return request.path === `/api/devices/${input.id}/channels/${input.channel}/play`;
        return request.path === "/api/play" && (input.kind === "rtsp" ? request.source?.source_id === input.id : request.source?.push_device_id === input.id);
      }));
      const first = await q(snapshot);
      assert(`${input.label}: 原生视频/音频统计与固定尺寸`, ready(first) && first.width === input.width && first.height === input.height,
        JSON.stringify(first));
      await b.sleep(2200);
      const last = await q(snapshot);
      assert(`${input.label}: 同 viewer 连续真实解码与音频能量增长`, ready(last) && last.peer === first.peer &&
        last.videoReport === first.videoReport && last.audioReport === first.audioReport &&
        last.width === input.width && last.height === input.height &&
        ["framesDecoded", "elementFrames", "videoBytes", "audioBytes", "samples", "duration", "energy"].every(field => last[field] > first[field]),
        JSON.stringify({ first, last }));
      assert(`${input.label}: 仅建立一个真实 viewer`, await open() === 1 && await q(
        `window.__multiInput.peers.length === ${before.peers + 1} && window.__multiInput.created.length === ${before.created + 1} && Boolean(window.__multiInput.created.at(-1).resource)`));
      await b.shot(`${shots}/multi-input-${input.kind === "gb" ? "gb" : `${input.protocol}-${input.kind === "push" ? "publish" : "pull"}`}.png`);
      const resource = await q("window.__multiInput.created.at(-1).resource");
      await q("document.querySelector('.detail-player [aria-label=关闭画面]').click()");
      assert(`${input.label}: 真实 DELETE 对应本 viewer 且 Peer 关闭`, await waitFor(b,
        `window.__multiInput.peers.every(peer => peer.signalingState === 'closed') && window.__multiInput.deleted.length === ${before.deleted + 1} && !document.querySelector('.detail-player video')`, 7000));
      const deleted = await q("window.__multiInput.deleted.at(-1)");
      assert(`${input.label}: viewer DELETE 服务端真实响应`, deleted.resource === resource && [200, 204, 404].includes(deleted.status), String(deleted.status));
      if (input.kind === "rtsp") {
        const response = await api("/api/sources");
        const current = response.body?.sources?.find(source => source.source_id === input.id);
        assert(`${input.label}: 关闭 viewer 不停止自动拉流`, response.status === 200 && current?.desired_state === "running" && current?.session?.state === "created" && current.session.media_ready);
      } else if (input.kind === "push") {
        const response = await api(`/api/push-devices/${input.id}`);
        assert(`${input.label}: 关闭 viewer 不停止发布`, response.status === 200 && response.body?.state === "publishing" && response.body.media_ready);
      } else {
        const live = (await channels()).find(channel => channel.channel_id === input.channel)?.live;
        assert(`${input.label}: 关闭 viewer 保留 GB 点播上游`, live?.state === "streaming");
      }
      results.push({ input: input.label, expected: { width: input.width, height: input.height, tracks: 2 }, first, last, deleteStatus: deleted.status });
      completed += 1;
    }
    if (process.env.MEDIA_LOG) {
      const deadline = Date.now() + 7000;
      while (unclosedServerSessions(logStart).length && Date.now() < deadline) await b.sleep(100);
      assert("七输入本轮服务端 WHEP viewer 全部关闭", unclosedServerSessions(logStart).length === 0);
    }
  } finally {
    // 路由离开走真实 UI closeViewer 清理；不替代关闭断言，不删除 source/device/共享 GB live。
    let closed = false;
    try {
      await q("location.hash = '#devices'");
      await b.sleep(700);
      closed = await waitFor(b, "Boolean(window.__multiInput) && window.__multiInput.peers.every(peer => peer.signalingState === 'closed')", 7000);
    } catch {
      // 保存真实清理失败，不能靠移除观察器或伪造关闭来把失败变成 PASS。
    }
    check("七输入测试离开详情无遗留 Peer", closed);
    check("七种输入全部完成真实双轨增长及 viewer DELETE", completed === cases.length, `${completed}/${cases.length}`);
    writeFileSync(`${shots}/multi-input-stats.json`, JSON.stringify({ completed, requested: cases.length, results }, null, 2) + "\n", { flag: "wx" });
    await b.removeBeforeLoad(injected);
    summary();
  }
}
