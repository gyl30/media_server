// 控制台、视频资源、系统状态。所有数值都由真实快照确定性汇总；没有数据来源的指标只标注暂不支持。
import { store, unifiedDevices, runningStreams, deviceHash, refresh, loadChannels, PUSH_PROTOCOLS } from "/store.js";
import { h, icon, badge, protoTag, emptyState, errorMessage, formatClock, replaceKeepingFocus } from "/ui.js";

function playItem(stream) {
  return { kind: stream.kind, deviceID: stream.deviceID, protocol: stream.protocol,
    key: stream.kind === "gb" ? `gb:${stream.target.device_id}:${stream.target.channel_id}` : `${stream.kind}:${stream.deviceID}`,
    label: stream.name, target: stream.target };
}

function loadingOrError(root) {
  if (store.loaded) {
    return false;
  }
  root.replaceChildren(h("div", { class: "card" }, store.error
    ? emptyState({ icon: "wifi-off", tone: "danger", title: "暂时无法获取数据", text: errorMessage(store.error),
      action: h("button", { class: "btn", type: "button", onclick: () => void refresh() }, icon("refresh"), "重试") })
    : emptyState({ icon: "clock", title: "正在加载…" })));
  return true;
}

// GB 点播状态来自通道列表：每次打开页面为在线设备取一次，之后由手动刷新更新，不随轮询逐台请求。
function onlineChannelsOnce() {
  let done = false;
  return () => {
    if (done || !store.loaded) return;
    done = true;
    const online = store.devices.filter(device => device.online).map(device => device.device_id);
    if (online.length) void loadChannels(online);
  };
}

// 通道数据时间：取已缓存通道中最早的一次请求时间；没有在线设备时为空。
function channelsNote() {
  const online = store.devices.filter(device => device.online);
  if (online.length === 0) {
    return "";
  }
  const loaded = online.map(device => store.channels.get(device.device_id)).filter(Boolean);
  if (loaded.length < online.length) {
    return "GB 通道数据获取中";
  }
  if (loaded.some(item => item.error)) {
    return "部分 GB 设备通道获取失败，点击右上角刷新重试";
  }
  return `GB 通道数据时间 ${formatClock(Math.min(...loaded.map(item => item.at)))}，点击右上角刷新更新`;
}

function summaryOf() {
  const devices = unifiedDevices();
  const streams = runningStreams();
  const gb = devices.filter(device => device.kind === "gb");
  const channels = gb.flatMap(device => device.channels || []);
  return {
    devices, streams, gb, channels,
    rtsp: devices.filter(device => device.kind === "rtsp"),
    push: devices.filter(device => device.kind === "push"),
    attention: devices.filter(device => (device.kind === "gb" && !device.raw.online) || device.status.group === "attention"),
    online: devices.filter(device => device.status.group === "online").length,
  };
}

function metric(label, glyph, value, unit, foot) {
  return h("div", { class: "card kpi" }, h("div", { class: "label" }, label), h("span", { class: "chip" }, icon(glyph)),
    h("div", { class: "value" }, value, unit ? h("small", {}, unit) : null), h("div", { class: "meta" }, foot));
}

export function mountOverview(root, ctx) {
  const ensureChannels = onlineChannelsOnce();
  const body = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "系统总览"), h("p", {}, "快速掌握设备接入、视频资源与运行概况。数据来自设备、拉流源、推流设备与通道接口。")),
      h("div", { class: "actions" }, h("a", { class: "btn primary", href: "#monitor" }, icon("video"), "进入实时监控"))),
    body));

  function update() {
    ensureChannels();
    if (loadingOrError(body)) {
      return;
    }
    const { devices, streams, gb, channels, rtsp, push, attention } = summaryOf();
    const count = kind => streams.filter(stream => stream.kind === kind).length;
    const pushProtocols = Object.entries(PUSH_PROTOCOLS).map(([value, label]) => `${label} ${push.filter(device => device.raw.protocol === value).length}`).join(" / ");
    const note = channelsNote();
    replaceKeepingFocus(body, [
      h("div", { class: "kpis" },
        metric("接入对象", "camera", String(devices.length), "个", `GB28181 ${gb.length} · RTSP 拉流 ${rtsp.length} · 推流 ${push.length}`),
        metric("在线国标设备", "check", String(gb.filter(device => device.raw.online).length), `/ ${gb.length}`, "按 SIP 注册与心跳判断"),
        metric("国标通道", "grid", String(channels.length), "个", note || "没有在线的国标设备"),
        metric("运行中视频源", "video", String(streams.length), "路", `GB 点播 ${count("gb")} · RTSP 拉流 ${count("rtsp")} · 推流 ${count("push")}`)),
      h("div", { class: "overview-grid" },
        h("div", { class: "stack" },
          h("section", { class: "card" },
            h("div", { class: "card-head" }, h("h2", {}, "接入方式"), h("a", { class: "link", href: "#devices" }, "设备管理", icon("arrow-right", "sm"))),
            h("div", { class: "card-body stack" },
              h("div", { class: "connect-overview" },
                connectType("camera", "GB28181", gb.length, `在线 ${gb.filter(device => device.raw.online).length} 台`),
                connectType("link", "RTSP 拉流", rtsp.length, `拉流中 ${count("rtsp")} 路`),
                connectType("radio", "主动推流", push.length, pushProtocols)),
              h("div", { class: "hint" }, "设备与视频源统一管理，但不同接入方式使用各自真实的配置项；只有国标设备具有 SIP 注册与通道层级。"))),
          h("section", { class: "card" },
            h("div", { class: "card-head" }, h("h2", {}, "需要关注"), h("span", { class: "muted tiny" }, `${attention.length} 项`)),
            attention.length === 0
              ? emptyState({ icon: "check", title: "一切正常", text: "没有离线的国标设备或异常的拉流源。" })
              : h("div", { class: "event-list" }, attention.map(device => h("div", { class: "event" },
                h("span", { class: "sev", style: `background:${device.status.tone === "warn" ? "var(--warn)" : "#b8c3cf"}` }),
                h("div", { class: "what" }, h("strong", {}, device.name), h("span", {}, `${device.protocol} · ${device.status.label}${device.status.detail ? ` · ${device.status.detail}` : ""}`)),
                h("a", { class: "link", href: deviceHash(device.kind, device.id) }, "查看")))))),
        h("section", { class: "card" },
          h("div", { class: "card-head" }, h("h2", {}, "正在运行的视频"), h("a", { class: "link", href: "#resources" }, "视频资源", icon("arrow-right", "sm"))),
          streams.length === 0
            ? emptyState({ icon: "video", title: "当前没有运行中的视频源", text: "国标点播、开始拉流或推流设备接入后会出现在这里。" })
            : h("div", { class: "event-list" }, streams.slice(0, 8).map(stream => h("div", { class: "event" },
              h("span", { class: "feed-icon" }, icon("camera")),
              h("div", { class: "what" }, h("strong", {}, stream.name), h("span", {}, `${stream.protocol} · ${stream.kind === "gb" ? "国标点播" : stream.kind === "rtsp" ? "RTSP 拉流" : "推流"}`)),
              h("button", { class: "link", type: "button", dataset: { key: `play:${stream.streamID}` }, onclick: () => ctx.playInCenter(playItem(stream)) }, "预览")))))),
    ]);
  }

  update();
  return { update, unmount() {} };
}

function connectType(glyph, title, total, foot) {
  return h("a", { class: "connect-type", href: "#devices" }, icon(glyph, "lg"), h("strong", {}, title), h("span", { class: "number" }, String(total)), h("small", {}, foot));
}

export function mountStreams(root, ctx) {
  const ensureChannels = onlineChannelsOnce();
  let kind = "all";
  const filter = h("select", { class: "select", "aria-label": "按来源筛选", onchange: event => { kind = event.target.value; update(); } },
    h("option", { value: "all" }, "全部来源"), h("option", { value: "gb" }, "GB28181 点播"), h("option", { value: "rtsp" }, "RTSP 拉流"), h("option", { value: "push" }, "推流"));
  const count = h("span", { class: "muted tiny" });
  const body = h("div");
  const foot = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "视频资源"), h("p", {}, "统一查看各接入方式当前正在运行、可以播放的视频源。")),
      h("div", { class: "actions" }, h("a", { class: "btn primary", href: "#monitor" }, icon("video"), "打开实时监控"))),
    h("div", { class: "notice", style: "margin-bottom:18px" }, icon("info"), "视频源是媒体资源，不等同于 GB28181 设备的注册状态；推流设备的“推流中”表示接入地址已通过认证，画面以实际预览为准。"),
    h("div", { class: "card" },
      h("div", { class: "toolbar" }, h("div", { class: "toolbar-left" }, filter), h("div", { class: "toolbar-right" }, count)),
      body, foot)));

  function update() {
    ensureChannels();
    if (loadingOrError(body)) {
      return;
    }
    const all = runningStreams();
    const streams = all.filter(stream => kind === "all" || stream.kind === kind);
    count.textContent = `共 ${streams.length} 路`;
    foot.replaceChildren(h("div", { class: "table-foot" }, h("span", {}, channelsNote() || "没有在线的国标设备"), h("span", {}, "轨道、码率与观看人数需要媒体服务器提供统计接口，暂不支持")));
    if (streams.length === 0) {
      body.replaceChildren(emptyState({ icon: "video", title: "当前没有运行中的视频源", text: "在设备详情中开始点播、开始拉流，或让推流设备推流后，视频源会出现在这里。" }));
      return;
    }
    replaceKeepingFocus(body, [h("div", { class: "table-scroll" }, h("table", { class: "table cards" },
      h("thead", {}, h("tr", {}, h("th", {}, "视频名称"), h("th", {}, "协议"), h("th", { class: "c-mode" }, "来源"), h("th", {}, "当前状态"), h("th", {}, "操作"))),
      h("tbody", {}, streams.map(stream => h("tr", {},
        h("td", { class: "c-name" }, h("div", { class: "cell-title" }, h("strong", {}, stream.name), h("span", { class: "table-sub mono", title: stream.streamID }, stream.streamID))),
        h("td", { class: "c-proto" }, protoTag(stream.protocol)),
        h("td", { class: "c-mode" }, stream.kind === "gb" ? "国标点播" : stream.kind === "rtsp" ? "RTSP 拉流" : "主动推流"),
        h("td", { class: "c-status" }, badge(stream.kind === "push" ? "推流中" : "运行中", "success")),
        h("td", { class: "c-actions" }, h("div", { class: "text-actions" },
          h("button", { type: "button", dataset: { key: `play:${stream.streamID}` }, onclick: () => ctx.playInCenter(playItem(stream)) }, "加入监控"),
          h("a", { href: deviceHash(stream.kind, stream.deviceID) }, "播放地址")))))))),
    ]);
  }

  update();
  return { update, unmount() {} };
}

const UNSUPPORTED = [
  ["服务运行信息", ["媒体服务器版本与运行时间", "信令服务运行时间", "当前网络连接数"]],
  ["资源监控", ["CPU 使用率", "内存占用", "网络吞吐", "活跃播放会话"]],
];

export function mountStatus(root) {
  const ensureChannels = onlineChannelsOnce();
  const body = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "系统状态"), h("p", {}, "平台状态与诊断信息，遵循实际后端能力边界。"))),
    h("div", { class: "notice", style: "margin-bottom:18px" }, icon("shield"), "平台目前没有提供 CPU、内存、网络带宽和实时会话的统计接口，这些指标不显示数值，也不由浏览器估算。"),
    body));

  function update() {
    ensureChannels();
    if (loadingOrError(body)) {
      return;
    }
    const { devices, streams, gb, rtsp, push, online } = summaryOf();
    const line = (label, value) => h("div", { class: "status-line" }, h("span", {}, label), value);
    replaceKeepingFocus(body, [h("div", { class: "status-grid" },
      h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "接口与数据")),
        h("div", { class: "card-body" },
          line("signaling 接口", store.error ? badge("最近一次刷新失败", "danger") : badge("正常", "success")),
          line("最近数据时间", h("strong", {}, formatClock(store.updatedAt))),
          line("刷新失败原因", h("span", {}, store.error ? errorMessage(store.error) : "—")),
          line("GB 通道数据", h("span", { class: "muted tiny" }, channelsNote() || "没有在线的国标设备")))),
      h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "接入统计")),
        h("div", { class: "card-body" },
          line("在线 / 运行中接入对象", h("strong", {}, `${online} / ${devices.length}`)),
          line("国标设备在线", h("strong", {}, `${gb.filter(device => device.raw.online).length} / ${gb.length}`)),
          line("RTSP 拉流中", h("strong", {}, `${streams.filter(stream => stream.kind === "rtsp").length} / ${rtsp.length}`)),
          line("推流中", h("strong", {}, `${streams.filter(stream => stream.kind === "push").length} / ${push.length}`)),
          line("国标点播", h("strong", {}, String(streams.filter(stream => stream.kind === "gb").length))))),
      ...UNSUPPORTED.map(([title, items]) => h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, title)),
        h("div", { class: "card-body" }, items.map(item => line(item, h("span", { class: "unavailable" }, "暂不支持"))))))),
    h("div", { class: "hint", style: "margin-top:18px" }, "后端提供真实的只读统计接口后，资源监控可以逐项启用；当前不把估算或模拟数字展示为实测。")]);
  }

  update();
  return { update, unmount() {} };
}
