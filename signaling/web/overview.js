import { store, unifiedDevices, runningStreams, deviceHash, refresh, loadChannels } from "/store.js";
import { h, icon, badge, emptyState, errorMessage, formatClock, replaceKeepingFocus } from "/ui.js";

function playItem(stream) {
  return { kind: stream.kind, deviceID: stream.deviceID, protocol: stream.protocol,
    key: stream.kind === "gb" ? `gb:${stream.target.device_id}:${stream.target.channel_id}` : `${stream.kind}:${stream.deviceID}`,
    label: stream.name, owner: stream.owner, target: stream.target };
}

function channelsTime() {
  const loaded = store.devices.filter(device => device.online).map(device => store.channels.get(device.device_id)).filter(Boolean);
  return loaded.length ? `通道数据 ${formatClock(Math.min(...loaded.map(item => item.at)))}` : "";
}

function stat(label, value) {
  return h("div", { class: "stat" }, h("span", {}, label), h("strong", {}, value));
}

export function mountOverview(root, ctx) {
  let channelsRequested = false;
  const body = h("div", { class: "page" });
  root.replaceChildren(body);

  function update() {
    if (!store.loaded) {
      body.replaceChildren(h("section", { class: "card" }, store.error
        ? emptyState({ icon: "alert", tone: "danger", title: errorMessage(store.error),
          action: h("button", { class: "btn", type: "button", onclick: () => void refresh() }, icon("refresh"), "重试") })
        : emptyState({ icon: "clock", title: "正在加载…" })));
      return;
    }
    if (!channelsRequested) {
      channelsRequested = true;
      const online = store.devices.filter(device => device.online).map(device => device.device_id);
      if (online.length) void loadChannels(online);
    }
    const devices = unifiedDevices();
    const streams = runningStreams();
    const attention = devices.filter(device => device.status.group === "attention" || (device.kind === "gb" && !device.raw.online));
    const channels = devices.filter(device => device.kind === "gb").flatMap(device => device.channels || []);
    replaceKeepingFocus(body, [
      h("section", { class: "card stats" },
        stat("设备", String(devices.length)),
        stat("在线", String(devices.filter(device => device.status.group === "online").length)),
        stat("国标通道", String(channels.length)),
        stat("运行中的视频", String(streams.length))),
      h("div", { class: "overview-grid" },
        h("section", { class: "card" },
          h("div", { class: "card-head" }, h("h2", {}, "需要关注"), h("span", { class: "muted" }, String(attention.length))),
          attention.length === 0
            ? emptyState({ icon: "check", title: "没有需要关注的设备" })
            : h("ul", { class: "list" }, attention.map(device => h("li", {},
              h("div", { class: "list-main" }, h("a", { href: deviceHash(device.kind, device.id) }, device.name),
                h("span", { class: "muted" }, device.type)),
              h("div", { class: "list-side" }, badge(device.status.label, device.status.tone),
                device.status.detail ? h("span", { class: "muted" }, device.status.detail) : null))))),
        h("section", { class: "card" },
          h("div", { class: "card-head" }, h("h2", {}, "运行中的视频"), h("span", { class: "muted" }, channelsTime())),
          streams.length === 0
            ? emptyState({ icon: "video", title: "没有运行中的视频" })
            : h("ul", { class: "list" }, streams.map(stream => h("li", {},
              h("div", { class: "list-main" }, h("a", { href: deviceHash(stream.kind, stream.deviceID) }, stream.name),
                stream.owner ? h("span", { class: "muted" }, stream.owner) : null, h("span", { class: "muted" }, stream.type)),
              h("button", { class: "btn sm", type: "button", dataset: { key: `play:${stream.streamID}` }, onclick: () => ctx.playInCenter(playItem(stream)) },
                icon("play", "sm"), "播放")))))),
    ]);
  }

  update();
  return { update, unmount() {} };
}
