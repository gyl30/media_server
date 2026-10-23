import { api } from "/api.js";
import { formatAgo, formatClock, formatDuration } from "/ui.js";

export const store = {
  devices: [],
  sources: [],
  pushDevices: [],
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

export async function refresh() {
  const current = ++epoch;
  controller?.abort();
  const own = new AbortController();
  controller = own;
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

export function syncChannels(deviceIDs) {
  const stale = deviceIDs.filter(id => (store.channels.get(id)?.at ?? -1) < store.updatedAt);
  if (stale.length) void loadChannels(stale);
}

export function channelsOf(deviceID) {
  return store.channels.get(deviceID)?.channels;
}

export function deviceHash(kind, id) {
  return `#devices/${kind}/${encodeURIComponent(id)}`;
}

export const PUSH_PROTOCOLS = { rtmp: "RTMP", rtsp: "RTSP", whip: "WHIP" };
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
    detail: device.last_seen ? `最后心跳 ${formatAgo(Date.parse(device.last_seen))}` : "",
  };
}

export function sourceReady(source) {
  return source.session?.state === "created" && source.session.media_ready;
}

export function sourceStatus(source) {
  if (sourceReady(source)) {
    return { label: "拉流中", tone: "success", group: "online", detail: "" };
  }
  if (source.session) {
    return { label: "正在连接", tone: "pending", group: "online", detail: "" };
  }
  if (source.desired_state === "running") {
    return { label: "连接中断", tone: "danger", group: "attention", detail: "上游不可达、认证失败或已断开" };
  }
  return { label: "已停止", tone: "neutral", group: "offline", detail: "" };
}

const PUSH_STATUS = {
  publishing: { label: "推流中", tone: "success", group: "online" },
  pending: { label: "待接入", tone: "pending", group: "offline" },
  used: { label: "未推流", tone: "neutral", group: "offline", detail: "地址已使用，重新生成后可再次接入" },
  expired: { label: "地址已过期", tone: "warn", group: "attention", detail: "重新生成后可接入" },
};

export function pushStatus(device) {
  const status = PUSH_STATUS[device.state];
  if (device.state === "publishing") {
    const detail = `已接入 ${formatDuration(Date.now() - Date.parse(device.used_at))}`;
    return device.media_ready ? { ...status, detail } : { label: "等待媒体", tone: "pending", group: "online", detail };
  }
  if (device.state === "pending") {
    return { ...status, detail: `${formatClock(device.expires_at)} 前首次接入有效` };
  }
  return status;
}

export function pushURLs(device) {
  return [["rtmp", "RTMP", device.rtmp_url], ["rtsp", "RTSP", device.rtsp_url], ["whip", "WHIP", device.whip_url]]
    .filter(([value]) => value === device.protocol);
}

export function unifiedDevices() {
  return [
    ...store.devices.map(device => ({
      key: `gb:${device.device_id}`, kind: "gb", id: device.device_id, name: device.name, protocol: "GB28181", type: "GB28181",
      endpoint: device.device_id, raw: device, status: gbStatus(device), channels: channelsOf(device.device_id),
      channelError: store.channels.get(device.device_id)?.error || null,
    })),
    ...store.sources.map(source => ({
      key: `rtsp:${source.source_id}`, kind: "rtsp", id: source.source_id, name: source.stream_name,
      protocol: PULL_PROTOCOLS[pullProtocol(source.url)], type: `${PULL_PROTOCOLS[pullProtocol(source.url)]} 拉流`,
      endpoint: source.url, raw: source, status: sourceStatus(source),
    })),
    ...store.pushDevices.map(device => ({
      key: `push:${device.device_id}`, kind: "push", id: device.device_id, name: device.name,
      protocol: PUSH_PROTOCOLS[device.protocol], type: `${PUSH_PROTOCOLS[device.protocol]} 推流`,
      endpoint: pushURLs(device)[0]?.[2], raw: device, status: pushStatus(device),
    })),
  ].sort((a, b) => a.name.localeCompare(b.name, "zh-CN"));
}

export function runningStreams() {
  const streams = [];
  for (const device of store.devices) {
    for (const channel of channelsOf(device.device_id) || []) {
      if (channel.live?.media_ready) {
        streams.push({
          kind: "gb", protocol: "GB28181", type: "GB28181", streamID: channel.live.live_id, deviceID: device.device_id, name: channel.name || channel.channel_id,
          owner: device.name, target: { device_id: device.device_id, channel_id: channel.channel_id, live_id: channel.live.live_id },
        });
      }
    }
  }
  for (const source of store.sources) {
    if (sourceReady(source)) {
      const protocol = PULL_PROTOCOLS[pullProtocol(source.url)];
      streams.push({ kind: "rtsp", protocol, type: `${protocol} 拉流`, streamID: source.session.stream_id, deviceID: source.source_id, name: source.stream_name, target: { source_id: source.source_id } });
    }
  }
  for (const device of store.pushDevices) {
    if (device.media_ready) {
      streams.push({ kind: "push", protocol: PUSH_PROTOCOLS[device.protocol], type: `${PUSH_PROTOCOLS[device.protocol]} 推流`, streamID: device.token, deviceID: device.device_id, name: device.name, target: { push_device_id: device.device_id } });
    }
  }
  return streams;
}
