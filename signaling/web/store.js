// 设备快照：三个列表接口 + 在线 GB 设备的通道。统一设备与运行中媒体流都由快照确定性地推导，
// 不保存任何服务端没有的字段。
import { api } from "/api.js";
import { formatAgo, formatDuration } from "/ui.js";

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

export function subscribe(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
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
    const online = (devices.devices || []).filter(device => device.online);
    const channelResults = await Promise.allSettled(online.map(device => api.channels(device.device_id, own.signal)));
    if (current !== epoch) {
      return;
    }
    store.sources = sources.sources || [];
    store.devices = devices.devices || [];
    store.pushDevices = pushDevices.devices || [];
    const channels = new Map();
    online.forEach((device, index) => {
      const result = channelResults[index];
      // 单个设备的通道请求失败时沿用上一次结果，不把它当成"没有通道"。
      const previous = store.channels.get(device.device_id);
      if (result.status === "fulfilled") {
        channels.set(device.device_id, result.value.channels || []);
      } else if (previous) {
        channels.set(device.device_id, previous);
      }
    });
    store.channels = channels;
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
      listeners.forEach(listener => listener());
    }
  }
}

export function stopRefresh() {
  epoch += 1;
  controller?.abort();
  controller = null;
}

// 设备详情路由：#devices/<kind>/<id>。source_id 与推流设备 ID 都是 UUID，需要 kind 区分。
export function deviceHash(kind, id) {
  return `#devices/${kind}/${encodeURIComponent(id)}`;
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

const SESSION_STATUS = {
  created: { label: "取流中", tone: "success", group: "online" },
  starting: { label: "正在连接", tone: "pending", group: "online" },
  stopping: { label: "正在停止", tone: "pending", group: "online" },
  unresolved: { label: "正在清理", tone: "pending", group: "attention" },
};

export function sourceStatus(source) {
  if (source.session) {
    return { ...(SESSION_STATUS[source.session.state] || SESSION_STATUS.unresolved), detail: "" };
  }
  if (source.desired_state === "running") {
    return { label: "未连接", tone: "warn", group: "attention", detail: "已请求取流，当前没有运行中的拉流会话" };
  }
  return { label: "已停止", tone: "neutral", group: "offline", detail: "按需开始取流" };
}

export function pushStatus(device) {
  if (device.state === "verified") {
    return { label: "推流中", tone: "success", group: "online", detail: device.verified_at ? `已推流 ${formatDuration(Date.now() - Date.parse(device.verified_at))}` : "" };
  }
  if (device.state === "issued") {
    return { label: "等待推流", tone: "pending", group: "offline", detail: device.expires_at ? `地址 ${new Date(device.expires_at).toLocaleTimeString("zh-CN", { hour12: false })} 前首次连接有效` : "" };
  }
  return { label: "空闲", tone: "neutral", group: "offline", detail: "签发推流地址后设备才能推流" };
}

export function unifiedDevices() {
  return [
    ...store.devices.map(device => ({
      kind: "gb", id: device.device_id, name: device.name, access: "设备注册", raw: device, status: gbStatus(device),
      channels: device.online ? store.channels.get(device.device_id) : undefined,
    })),
    ...store.sources.map(source => ({
      kind: "rtsp", id: source.source_id, name: source.stream_name, access: "拉流", raw: source, status: sourceStatus(source),
      address: rtspHost(source.url),
    })),
    ...store.pushDevices.map(device => ({
      kind: "push", id: device.device_id, name: device.name, access: "推流 · RTMP / RTSP / WHIP", raw: device, status: pushStatus(device),
    })),
  ];
}

function rtspHost(value) {
  try {
    return new URL(value).host;
  } catch {
    return "";
  }
}

// 当前可以证明正在运行的媒体流：GB 点播 streaming、RTSP 会话 created、推流 verified。
// 播放目标沿用 whep.js 的 target 结构。
export function runningStreams() {
  const streams = [];
  for (const device of store.devices) {
    for (const channel of store.channels.get(device.device_id) || []) {
      if (channel.live?.state === "streaming") {
        streams.push({
          kind: "gb", streamID: channel.live.live_id, deviceID: device.device_id, name: `${device.name} · ${channel.name || channel.channel_id}`,
          detail: channel.channel_id, target: { device_id: device.device_id, channel_id: channel.channel_id, live_id: channel.live.live_id },
        });
      }
    }
  }
  for (const source of store.sources) {
    if (source.session?.state === "created") {
      streams.push({ kind: "rtsp", streamID: source.session.stream_id, deviceID: source.source_id, name: source.stream_name, detail: "RTSP 拉流", target: { source_id: source.source_id } });
    }
  }
  for (const device of store.pushDevices) {
    if (device.state === "verified") {
      streams.push({ kind: "push", streamID: device.token, deviceID: device.device_id, name: device.name, detail: "推流", target: { push_device_id: device.device_id } });
    }
  }
  return streams;
}
