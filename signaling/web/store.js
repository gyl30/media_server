// 设备快照：每 2.5 秒请求三个列表接口；GB 通道只在页面需要时按设备请求（loadChannels），不随轮询全量刷新。
// 统一设备与运行中媒体流都由快照确定性推导，不保存服务端没有的字段。
import { api } from "/api.js";
import { formatAgo, formatDuration } from "/ui.js";

export const store = {
  devices: [],
  sources: [],
  pushDevices: [],
  // GB 设备 ID → { channels, at, error }；at 为最近一次通道请求发出的时间，失败时保留上次成功的通道。
  channels: new Map(),
  loaded: false,
  error: null,
  updatedAt: 0,
};

const listeners = new Set();
let epoch = 0;
let controller = null;
const channelRequests = new Map();

export function subscribe(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function notify() {
  listeners.forEach(listener => listener());
}

export function refreshing() {
  return Boolean(controller);
}

// 新的刷新会中止旧刷新；只接受最新一次的结果。
export async function refresh() {
  const current = ++epoch;
  controller?.abort();
  const own = new AbortController();
  controller = own;
  // 快照时间取请求发出时刻：晚于某个时刻发出的快照才能证明那之后的状态。
  const startedAt = Date.now();
  try {
    const [sources, devices, pushDevices] = await Promise.all([
      api.sources(own.signal), api.devices(own.signal), api.pushDevices(own.signal),
    ]);
    if (current !== epoch) {
      return;
    }
    store.sources = sources.sources || [];
    store.devices = devices.devices || [];
    store.pushDevices = pushDevices.devices || [];
    // 已删除设备的通道缓存不再保留。
    for (const id of store.channels.keys()) {
      if (!store.devices.some(device => device.device_id === id)) store.channels.delete(id);
    }
    store.loaded = true;
    store.error = null;
    store.updatedAt = startedAt;
  } catch (error) {
    if (current !== epoch || error.name === "AbortError") {
      return;
    }
    store.error = error;
  } finally {
    if (current === epoch) {
      controller = null;
      notify();
    }
  }
}

export function stopRefresh() {
  epoch += 1;
  controller?.abort();
  controller = null;
}

// 请求指定 GB 设备的通道列表。同一设备已有请求在途时复用。
// 失败也记录请求时间：syncChannels 据此等到下一次快照再重试，避免在通知里立即重复请求。
export async function loadChannels(deviceIDs) {
  const pending = deviceIDs.map(id => {
    if (!channelRequests.has(id)) {
      const startedAt = Date.now();
      channelRequests.set(id, api.channels(id)
        .then(result => { store.channels.set(id, { channels: result.channels || [], at: startedAt, error: null }); })
        .catch(error => { store.channels.set(id, { channels: store.channels.get(id)?.channels, at: startedAt, error }); })
        .finally(() => channelRequests.delete(id)));
    }
    return channelRequests.get(id);
  });
  if (pending.length) {
    await Promise.all(pending);
    notify();
  }
}

// 正在查看的设备随轮询保持新鲜：只重新请求早于最近一次列表快照的通道缓存。
// 通道请求完成也会通知订阅者，按时间判断可避免在通知里反复请求。
export function syncChannels(deviceIDs) {
  const stale = deviceIDs.filter(id => (store.channels.get(id)?.at ?? -1) < store.updatedAt);
  if (stale.length) void loadChannels(stale);
}

export function channelsOf(deviceID) {
  return store.channels.get(deviceID)?.channels;
}

// 设备详情路由：#devices/<kind>/<id>。source_id 与推流设备 ID 都是 UUID，需要 kind 区分。
export function deviceHash(kind, id) {
  return `#devices/${kind}/${encodeURIComponent(id)}`;
}

export const PUSH_PROTOCOLS = { rtmp: "RTMP", rtsp: "RTSP", whip: "WHIP" };
// 主动拉流的协议由来源 URL 决定，不单独保存；WHEP 端点是 http:// 地址。
export const PULL_PROTOCOLS = { rtsp: "RTSP", rtmp: "RTMP", whep: "WHEP" };

export function pullProtocol(url) {
  const scheme = url.slice(0, url.indexOf(":"));
  return scheme === "http" ? "whep" : scheme;
}

function gbStatus(device) {
  if (device.online) {
    return { label: "在线", tone: "success", group: "online", detail: device.last_seen ? `心跳 ${formatAgo(Date.parse(device.last_seen))}` : "" };
  }
  return {
    label: "离线", tone: "neutral", group: "offline",
    detail: device.last_seen ? `最后心跳 ${formatAgo(Date.parse(device.last_seen))}` : "本次运行尚未收到注册",
  };
}

// 主动拉流：desired_state 是持久化的接入意图，session 是运行事实；created 表示媒体服务器已建立拉流会话，
// media_ready 表示媒体源已确定轨道（来自媒体对账，最多滞后一个对账周期）。
export function sourceReady(source) {
  return source.session?.state === "created" && source.session.media_ready;
}

export function sourceStatus(source) {
  if (sourceReady(source)) {
    return { label: "拉流中", tone: "success", group: "online", detail: "媒体源已建立" };
  }
  if (source.session?.state === "created") {
    return { label: "接入中", tone: "pending", group: "online", detail: "已连接上游，等待音视频参数" };
  }
  if (source.session) {
    return { label: "正在连接", tone: "pending", group: "online", detail: "正在建立拉流会话" };
  }
  if (source.desired_state === "running") {
    return { label: "连接失败或已结束", tone: "danger", group: "attention", detail: "上游不可达、认证失败或已断开，可修改地址或重试连接" };
  }
  return { label: "已停止", tone: "neutral", group: "offline", detail: "拉流已手动停止" };
}

// 推流设备：publishing 只说明接入地址已通过认证、推流运行已登记，不代表音视频健康。
const PUSH_STATUS = {
  publishing: { label: "推流中", tone: "success", group: "online" },
  pending: { label: "待接入", tone: "pending", group: "offline" },
  used: { label: "地址已使用", tone: "neutral", group: "offline", detail: "当前没有推流，重新生成地址后可再次接入" },
  expired: { label: "地址已过期", tone: "warn", group: "attention", detail: "重新生成地址后可接入" },
};

export function pushStatus(device) {
  const status = PUSH_STATUS[device.state];
  if (device.state === "publishing") {
    return { ...status, detail: `已接入 ${formatDuration(Date.now() - Date.parse(device.used_at))}` };
  }
  if (device.state === "pending") {
    return { ...status, detail: `${formatClock(device.expires_at)} 前首次接入有效` };
  }
  return status;
}

export function formatClock(value) {
  return new Date(value).toLocaleTimeString("zh-CN", { hour12: false });
}

// 推流设备按所选协议展示接入地址。
export function pushURLs(device) {
  return [["rtmp", "RTMP", device.rtmp_url], ["rtsp", "RTSP", device.rtsp_url], ["whip", "WHIP", device.whip_url]]
    .filter(([value]) => value === device.protocol);
}

// 前端统一展示对象。key 在三类资源间唯一；protocol 是显示用协议名，filter 是协议筛选值。
export function unifiedDevices() {
  return [
    ...store.devices.map(device => ({
      key: `gb:${device.device_id}`, kind: "gb", id: device.device_id, name: device.name, protocol: "GB28181", filter: "gb",
      mode: "SIP 注册", endpoint: device.device_id, raw: device, status: gbStatus(device), channels: channelsOf(device.device_id),
      channelError: store.channels.get(device.device_id)?.error || null,
    })),
    ...store.sources.map(source => ({
      key: `rtsp:${source.source_id}`, kind: "rtsp", id: source.source_id, name: source.stream_name,
      protocol: PULL_PROTOCOLS[pullProtocol(source.url)], filter: `${pullProtocol(source.url)}-pull`,
      mode: "拉流", endpoint: source.url, raw: source, status: sourceStatus(source),
    })),
    ...store.pushDevices.map(device => ({
      key: `push:${device.device_id}`, kind: "push", id: device.device_id, name: device.name,
      protocol: PUSH_PROTOCOLS[device.protocol], filter: `${device.protocol}-push`,
      mode: "推流", endpoint: pushURLs(device)[0]?.[2], raw: device, status: pushStatus(device),
    })),
  ].sort((a, b) => a.name.localeCompare(b.name, "zh-CN"));
}

// 当前可以证明正在运行的媒体流：GB 点播 streaming（仅限已获取通道的设备）、RTSP 会话 created、推流 publishing。
// 播放目标沿用 whep.js 的 target 结构。
export function runningStreams() {
  const streams = [];
  for (const device of store.devices) {
    for (const channel of channelsOf(device.device_id) || []) {
      if (channel.live?.state === "streaming") {
        streams.push({
          kind: "gb", protocol: "GB28181", streamID: channel.live.live_id, deviceID: device.device_id, name: `${device.name} · ${channel.name || channel.channel_id}`,
          detail: channel.channel_id, target: { device_id: device.device_id, channel_id: channel.channel_id, live_id: channel.live.live_id },
        });
      }
    }
  }
  for (const source of store.sources) {
    if (sourceReady(source)) {
      const protocol = PULL_PROTOCOLS[pullProtocol(source.url)];
      streams.push({ kind: "rtsp", protocol, streamID: source.session.stream_id, deviceID: source.source_id, name: source.stream_name, detail: `${protocol} 拉流`, target: { source_id: source.source_id } });
    }
  }
  for (const device of store.pushDevices) {
    if (device.state === "publishing") {
      streams.push({ kind: "push", protocol: PUSH_PROTOCOLS[device.protocol] || "推流", streamID: device.token, deviceID: device.device_id, name: device.name, detail: "推流", target: { push_device_id: device.device_id } });
    }
  }
  return streams;
}
