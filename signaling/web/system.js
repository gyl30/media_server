// 系统总览、媒体流、运行状态。所有数值都由真实快照确定性汇总；没有数据来源的指标只说明暂不支持。
import { store, unifiedDevices, runningStreams, deviceHash, refresh } from "/store.js";
import { h, icon, badge, protoTag, emptyState, errorMessage, formatClock, replaceKeepingFocus } from "/ui.js";

const KIND_LABEL = { gb: "GB28181", rtsp: "RTSP 拉流", push: "推流设备" };
const KIND_COLOR = { gb: "var(--p-gb)", rtsp: "var(--p-rtsp)", push: "var(--p-rtmp)" };

function playItem(stream) {
  return { kind: stream.kind, deviceID: stream.deviceID, key: stream.kind === "gb" ? `gb:${stream.target.device_id}:${stream.target.channel_id}` : `${stream.kind}:${stream.deviceID}`,
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

function summaryOf() {
  const devices = unifiedDevices();
  const streams = runningStreams();
  const kinds = ["gb", "rtsp", "push"].map(kind => {
    const items = devices.filter(device => device.kind === kind);
    return { kind, total: items.length, online: items.filter(device => device.status.group === "online").length };
  });
  const attention = devices.filter(device => (device.kind === "gb" && !device.raw.online) || device.status.group === "attention");
  return { devices, streams, kinds, attention, online: devices.filter(device => device.status.group === "online").length };
}

export function mountOverview(root, ctx) {
  const body = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "系统总览"), h("p", {}, "设备接入与媒体运行概况，由设备、RTSP 源、推流设备和通道接口汇总，每 2.5 秒刷新。")),
      h("div", { class: "actions" },
        h("a", { class: "btn", href: "#video" }, icon("video"), "打开视频中心"),
        h("a", { class: "btn primary", href: "#devices" }, icon("devices"), "设备管理"))),
    body));

  function update() {
    if (loadingOrError(body)) {
      return;
    }
    const { devices, streams, kinds, attention, online } = summaryOf();
    const maxTotal = Math.max(1, ...kinds.map(item => item.total));
    const count = kind => streams.filter(stream => stream.kind === kind).length;
    replaceKeepingFocus(body, [
      h("div", { class: "kpis" },
        h("div", { class: "card kpi" }, h("div", { class: "label" }, icon("devices", "sm"), "已登记设备"),
          h("div", { class: "value" }, String(devices.length), h("small", {}, "台")),
          h("div", { class: "meta" }, kinds.map(item => `${KIND_LABEL[item.kind]} ${item.total}`).join(" · "))),
        h("div", { class: "card kpi" }, h("div", { class: "label" }, h("span", { class: "live-dot on" }), "在线 / 运行中"),
          h("div", { class: "value" }, String(online), h("small", {}, `/ ${devices.length}`)),
          h("div", { class: "meta" }, "GB 已注册在线、RTSP 有拉流会话、推流设备正在推流")),
        h("div", { class: "card kpi" }, h("div", { class: "label" }, icon("streams", "sm"), "运行中媒体流"),
          h("div", { class: "value" }, String(streams.length), h("small", {}, "路")),
          h("div", { class: "meta" }, `GB 点播 ${count("gb")} · RTSP 拉流 ${count("rtsp")} · 推流 ${count("push")}`)),
        h("div", { class: "card kpi" }, h("div", { class: "label" }, icon("alert", "sm"), "需要关注"),
          h("div", { class: `value${attention.length ? " danger-text" : ""}` }, String(attention.length), h("small", {}, "项")),
          h("div", { class: "meta" }, "GB 设备离线、RTSP 源未连接或正在清理"))),
      h("div", { class: "overview-grid" },
        h("div", { class: "stack" },
          h("section", { class: "card" },
            h("div", { class: "card-head" }, h("div", {}, h("h2", {}, "按接入类型"), h("div", { class: "sub" }, "实色为在线 / 运行中，浅色为总数"))),
            h("div", { class: "card-body dist" }, kinds.map(item => h("div", { class: "dist-row" }, protoTag(item.kind),
              h("div", { class: "dist-bar" }, h("b", { style: `width:${(item.total / maxTotal) * 100}%;background:${KIND_COLOR[item.kind]}` }),
                h("i", { style: `width:${(item.online / maxTotal) * 100}%;background:${KIND_COLOR[item.kind]}` })),
              h("div", { class: "dist-num" }, h("strong", {}, String(item.online)), ` / ${item.total}`))))),
          h("section", { class: "card" },
            h("div", { class: "card-head" }, h("h2", {}, "需要关注"), h("a", { class: "btn sm ghost", href: "#devices" }, "全部设备", icon("arrow-right", "sm"))),
            attention.length === 0
              ? emptyState({ icon: "check", title: "一切正常", text: "没有离线的 GB28181 设备或异常的 RTSP 源。" })
              : h("div", { class: "event-list" }, attention.map(device => h("div", { class: "event" },
                h("span", { class: "sev", style: `background:${device.status.tone === "warn" ? "var(--warn)" : "var(--line-strong)"}` }),
                h("div", { class: "what" }, h("strong", {}, device.name), h("span", {}, `${KIND_LABEL[device.kind]} · ${device.status.label}${device.status.detail ? ` · ${device.status.detail}` : ""}`)),
                h("a", { class: "btn xs", href: deviceHash(device.kind, device.id) }, "查看")))))),
        h("div", { class: "stack" },
          h("section", { class: "card" },
            h("div", { class: "card-head" }, h("h2", {}, `正在运行的媒体流${streams.length ? ` · ${streams.length}` : ""}`), h("a", { class: "btn sm ghost", href: "#streams" }, "全部", icon("arrow-right", "sm"))),
            streams.length === 0
              ? emptyState({ icon: "streams", title: "当前没有运行中的媒体流", text: "开始点播、开始取流或设备推流后会出现在这里。" })
              : h("div", { class: "event-list" }, streams.slice(0, 6).map(stream => h("div", { class: "event" },
                h("span", { class: "live-dot on" }),
                h("div", { class: "what" }, h("strong", {}, stream.name), h("span", { class: "mono" }, stream.streamID)),
                h("button", { class: "btn xs", type: "button", dataset: { key: `play:${stream.streamID}` }, onclick: () => ctx.playInCenter(playItem(stream)) }, icon("play", "sm"), "播放"))))),
          h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "快捷入口")),
            h("div", { class: "card-body quick" },
              h("a", { href: "#devices" }, icon("plus"), h("strong", {}, "添加设备"), h("span", {}, "GB28181、RTSP 拉流或推流设备")),
              h("a", { href: "#video" }, icon("grid-4"), h("strong", {}, "视频中心"), h("span", {}, "单画面或四分屏观看")),
              h("a", { href: "#streams" }, icon("streams"), h("strong", {}, "媒体流"), h("span", {}, "查看当前实际运行的流")))))),
    ]);
  }

  update();
  return { update, unmount() {} };
}

export function mountStreams(root, ctx) {
  let kind = "all";
  const filter = h("div", { class: "segmented", role: "group", "aria-label": "按来源筛选" });
  const body = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "媒体流"),
      h("p", {}, "当前可以由接口证明正在运行的媒体流：GB 点播、RTSP 拉流会话、正在推流的推流设备。观看请使用视频中心。"))),
    h("div", { class: "card table-card" }, h("div", { class: "toolbar" }, filter), body)));

  function update() {
    const all = store.loaded ? runningStreams() : [];
    replaceKeepingFocus(filter, [["all", "全部"], ["gb", "GB28181"], ["rtsp", "RTSP 拉流"], ["push", "推流"]].map(([value, label]) => h("button", {
      type: "button", class: kind === value ? "is-active" : "", "aria-pressed": String(kind === value), dataset: { key: `kind:${value}` },
      onclick: () => { kind = value; update(); },
    }, label, h("span", { class: "n" }, String(value === "all" ? all.length : all.filter(stream => stream.kind === value).length)))));
    if (loadingOrError(body)) {
      return;
    }
    const streams = all.filter(stream => kind === "all" || stream.kind === kind);
    if (streams.length === 0) {
      body.replaceChildren(emptyState({ icon: "streams", title: "当前没有运行中的媒体流", text: "在设备详情中开始点播、开始取流，或让推流设备推流后，媒体流会出现在这里。" }));
      return;
    }
    replaceKeepingFocus(body, [h("div", { class: "table-scroll" }, h("table", { class: "table cards" },
      h("thead", {}, h("tr", {}, h("th", {}, "来源"), h("th", {}, "媒体流 ID"), h("th", {}, "类型"), h("th", {}, "状态"), h("th", { class: "actions" }, "操作"))),
      h("tbody", {}, streams.map(stream => h("tr", {},
        h("td", { class: "c-name" }, h("div", { class: "cell-title" },
          h("a", { class: "link-btn", href: deviceHash(stream.kind, stream.deviceID), dataset: { key: `open:${stream.streamID}` } }, stream.name),
          h("span", { class: "direction" }, stream.kind === "gb" ? `通道 ${stream.detail}` : stream.detail))),
        h("td", { class: "c-id" }, h("span", { class: "mono", title: stream.streamID }, stream.streamID)),
        h("td", { class: "c-proto" }, protoTag(stream.kind)),
        h("td", { class: "c-status" }, badge("运行中", "success")),
        h("td", { class: "actions" }, h("div", { class: "actions-inner" },
          h("button", { class: "btn sm", type: "button", dataset: { key: `play:${stream.streamID}` }, onclick: () => ctx.playInCenter(playItem(stream)) }, icon("play", "sm"), "播放"),
          h("a", { class: "btn sm ghost", href: deviceHash(stream.kind, stream.deviceID) }, "设备", icon("chevron-right", "sm"))))))))),
    h("div", { class: "table-foot" }, h("span", {}, `${streams.length} 路运行中`), h("span", {}, "轨道、码率与观看数需要媒体服务器提供只读统计接口，暂不支持"))]);
  }

  update();
  return { update, unmount() {} };
}

const UNSUPPORTED = ["CPU 使用率", "内存占用", "网络连接数", "入站 / 出站带宽", "播放会话数", "各媒体流轨道与码率"];

export function mountStatus(root) {
  const body = h("div");
  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "运行状态"),
      h("p", {}, "只展示现有接口能够给出的数据。主机与媒体服务器的资源指标需要新增只读统计接口后才能提供。"))),
    body));

  function update() {
    if (loadingOrError(body)) {
      return;
    }
    const { devices, streams, kinds, online } = summaryOf();
    const lives = streams.filter(stream => stream.kind === "gb").length;
    const pulls = streams.filter(stream => stream.kind === "rtsp").length;
    const pushes = streams.filter(stream => stream.kind === "push").length;
    replaceKeepingFocus(body, [
      h("div", { class: "section-title" }, "现有接口可得", h("span", {}, "signaling 设备、RTSP 源、推流设备与通道接口")),
      h("div", { class: "status-grid" },
        h("div", { class: "card metric" }, h("div", { class: "top" }, "signaling 接口"),
          h("div", { class: "big" }, store.error ? badge("最近一次刷新失败", "danger") : badge("正常", "success")),
          h("div", { class: "muted small" }, store.error ? errorMessage(store.error) : `数据时间 ${formatClock(store.updatedAt)}`)),
        h("div", { class: "card metric" }, h("div", { class: "top" }, "设备在线 / 运行中"), h("div", { class: "big" }, String(online), h("small", {}, `/ ${devices.length}`)),
          h("div", { class: "muted small" }, kinds.map(item => `${KIND_LABEL[item.kind]} ${item.online}/${item.total}`).join(" · "))),
        h("div", { class: "card metric" }, h("div", { class: "top" }, "运行中媒体流"), h("div", { class: "big" }, String(streams.length), h("small", {}, "路")),
          h("div", { class: "muted small" }, `GB 点播 ${lives} · RTSP 拉流 ${pulls} · 推流 ${pushes}`))),
      h("div", { class: "section-title" }, "暂不支持", h("span", {}, "需要媒体服务器新增只读统计接口")),
      h("section", { class: "card" }, h("div", { class: "card-body" },
        h("p", { class: "muted" }, "以下指标当前没有数据来源，不显示数值："),
        h("ul", { class: "unsupported" }, UNSUPPORTED.map(name => h("li", {}, icon("cpu", "sm"), name))))),
    ]);
  }

  update();
  return { update, unmount() {} };
}
