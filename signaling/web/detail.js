// 设备详情：设备信息、GB28181 通道与点播、RTSP 取流控制、推流地址签发。
import { api } from "/api.js";
import { store, unifiedDevices, refresh, refreshing } from "/store.js";
import {
  h, icon, badge, protoTag, emptyState, toast, confirmAction, errorMessage, copyButton, formatAgo, replaceKeepingFocus,
} from "/ui.js";
import { openDeviceDialog, deleteDevice, editState } from "/devices.js";
import { createPlaybackPanel } from "/playback.js";

export function mountDetail(root, ctx, kind, id) {
  const pending = new Set();
  let channelID = "";
  let publish = null;
  const head = h("div", { class: "detail-head" });
  const left = h("div", { class: "detail-col" });
  const right = h("div", { class: "detail-col" });
  const playback = createPlaybackPanel();
  right.append(playback.element);
  const page = h("div", { class: "page" }, head, h("div", { class: "detail-grid" }, left, right));
  root.replaceChildren(page);

  const find = () => unifiedDevices().find(device => device.kind === kind && device.id === id);
  // 刚创建或通过链接打开的设备可能还不在快照里：只有挂载之后的快照仍找不到，才判定为不存在。
  const mountedAt = Date.now();

  // 同一资源的操作串行：进行中禁用相关按钮，完成后刷新快照。
  async function run(key, action, success) {
    if (pending.has(key)) {
      return;
    }
    pending.add(key);
    update();
    try {
      const result = await action();
      if (success) toast(success, "success");
      return result;
    } catch (error) {
      toast(errorMessage(error), "danger");
      return undefined;
    } finally {
      pending.delete(key);
      await refresh();
    }
  }

  function update() {
    const device = find();
    if (!device) {
      const confirmed = store.loaded && store.updatedAt >= mountedAt;
      if (!confirmed && !refreshing()) {
        void refresh();
      }
      root.replaceChildren(h("div", { class: "page" }, h("div", { class: "card" }, confirmed
        ? emptyState({ icon: "search", title: "设备不存在或已被删除", action: h("a", { class: "btn", href: "#devices" }, "返回设备管理") })
        : emptyState({ icon: "clock", title: "正在加载设备…" }))));
      return;
    }
    if (!root.contains(page)) {
      root.replaceChildren(page);
    }
    ctx.setTitle(device.name);
    if (kind === "gb") {
      const channels = device.channels || [];
      if (!channels.some(channel => channel.channel_id === channelID)) {
        channelID = (channels.find(channel => channel.live?.state === "streaming") || channels.find(channel => channel.status === "ON") || channels[0])?.channel_id || "";
      }
    }
    replaceKeepingFocus(head, headContent(device));
    replaceKeepingFocus(left, [infoCard(device),
      kind === "gb" ? channelCard(device) : null,
      kind === "push" ? publishCard(device) : null].filter(Boolean));
    playback.update(playbackState(device));
  }

  // 只有可证明正在运行的媒体流才申请播放地址；否则给出该设备当前可做的下一步。
  function playbackState(device) {
    const raw = device.raw;
    if (kind === "gb") {
      const channel = (device.channels || []).find(item => item.channel_id === channelID);
      const title = channel ? `${device.name} · ${channel.name || channel.channel_id}` : device.name;
      if (channel?.live?.state === "streaming") {
        return { title, stream: { id: channel.live.live_id, source: { live_id: channel.live.live_id } } };
      }
      return { title, notReady: gbNotReady(device, channel) };
    }
    if (kind === "rtsp") {
      if (raw.session?.state === "created") {
        return { title: device.name, stream: { id: raw.session.stream_id, source: { source_id: id } } };
      }
      const starting = raw.session ? "平台正在处理拉流会话，就绪后会自动申请播放地址。"
        : raw.desired_state === "running" ? "已请求取流，但当前没有运行中的拉流会话。请检查摄像机地址与认证信息后重新取流。"
        : "平台尚未连接这个 RTSP 源。开始取流后才会生成播放地址。";
      return { title: device.name, notReady: emptyState({ icon: raw.session ? "clock" : "film", title: "媒体未就绪", text: starting }) };
    }
    if (raw.state === "verified") {
      return { title: device.name, stream: { id: raw.token, source: { push_device_id: id } } };
    }
    return { title: device.name, notReady: emptyState({ icon: "film", title: "媒体未就绪 · 设备未推流", text: "设备使用推流地址连接并开始推流后，这里才会生成播放地址。" }) };
  }

  function gbNotReady(device, channel) {
    if (!device.raw.online) {
      return emptyState({ icon: "wifi-off", title: "媒体未就绪 · 设备离线", text: "设备注册上线后，可选择通道并开始点播。" });
    }
    if (!channel) {
      return emptyState({ icon: "layers", title: "没有可选通道", text: "设备尚未上报通道。" });
    }
    if (channel.status !== "ON") {
      return emptyState({ icon: "wifi-off", title: "媒体未就绪 · 通道离线", text: "该通道当前不可点播，请检查前端摄像机。" });
    }
    if (channel.live) {
      return emptyState({ icon: "clock", title: "点播进行中…", text: "媒体流就绪后会自动申请播放地址。" });
    }
    const busy = pending.has(`channel:${channel.channel_id}`);
    return emptyState({
      icon: "film", title: "媒体未就绪", text: "该通道当前没有运行中的媒体流。打开详情不会自动点播，需要时请手动开始。",
      action: h("button", { class: "btn primary", type: "button", disabled: busy, "aria-busy": String(busy), dataset: { key: "start-live" },
        onclick: () => void startLive(channel) }, busy ? null : icon("play", "sm"), "开始点播"),
    });
  }

  function headContent(device) {
    const editable = editState(device);
    const source = device.raw;
    const busy = pending.has("source");
    return [
      h("div", { class: "detail-title" }, h("h1", {}, device.name || "未命名"), h("div", { class: "line" },
        protoTag(device.kind), h("span", {}, device.access), badge(device.status.label, device.status.tone), h("span", { class: "mono" }, device.id))),
      h("div", { class: "head-actions" },
        kind === "rtsp" && !source.session
          ? h("button", { class: "btn primary", type: "button", disabled: busy, "aria-busy": String(busy), dataset: { key: "start" },
            onclick: () => run("source", () => api.startSource(id), "已请求开始取流") }, icon("play", "sm"), source.desired_state === "running" ? "重新取流" : "开始取流") : null,
        kind === "rtsp" && (source.session || source.desired_state === "running")
          ? h("button", { class: "btn danger-subtle", type: "button", disabled: busy, dataset: { key: "stop" }, onclick: () => stopSource(device) }, icon("square", "sm"), "停止取流") : null,
        kind !== "gb" ? h("button", { class: "btn", type: "button", disabled: !editable.ok, title: editable.reason, dataset: { key: "edit" },
          onclick: () => openDeviceDialog(device) }, icon("edit", "sm"), "编辑") : null,
        h("button", { class: "btn danger-subtle", type: "button", dataset: { key: "delete" }, onclick: () => deleteDevice(device, ctx) }, icon("trash", "sm"), "删除")),
    ];
  }

  function stopSource(device) {
    confirmAction({
      title: "停止取流", message: `停止「${device.name}」后，正在观看的用户会断开，已签发的播放地址随之失效。`, confirmLabel: "停止取流",
      async action() {
        await api.stopSource(id);
        toast("已停止取流", "success");
        void refresh();
      },
    });
  }

  function infoCard(device) {
    const raw = device.raw;
    const rows = [["设备 ID", h("span", { class: "mono" }, device.id)], ["接入类型", device.access],
      ["当前状态", [badge(device.status.label, device.status.tone), device.status.detail ? h("span", { class: "muted gap" }, device.status.detail) : null]]];
    if (kind === "gb") {
      rows.push(["最近心跳", raw.last_seen ? formatAgo(Date.parse(raw.last_seen)) : h("span", { class: "muted" }, "—")]);
      rows.push(["通道", raw.online ? `${(device.channels || []).length} 个，在线 ${(device.channels || []).filter(channel => channel.status === "ON").length} 个` : h("span", { class: "muted" }, "设备不在线，通道不可用")]);
    } else if (kind === "rtsp") {
      rows.push(["RTSP 地址", h("span", { class: "mono" }, raw.url)]);
      rows.push(["认证", raw.username ? [h("span", {}, "用户名 "), h("span", { class: "mono" }, raw.username), h("span", { class: "muted" }, " · 密码不回显")] : h("span", { class: "muted" }, "未设置")]);
      rows.push(["期望状态", raw.desired_state === "running" ? "取流" : "停止"]);
      rows.push(["当前媒体流", raw.session ? h("span", { class: "mono" }, raw.session.stream_id) : h("span", { class: "muted" }, "—")]);
    } else {
      rows.push(["推流协议", h("span", { class: "muted" }, "未指定（签发时同时提供 RTMP / RTSP / WHIP）")]);
    }
    return h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "设备信息")),
      h("div", { class: "card-body" }, h("dl", { class: "kv" }, rows.flatMap(([label, value]) => [h("dt", {}, label), h("dd", {}, value)]))));
  }

  function channelCard(device) {
    const channels = device.channels || [];
    let content;
    if (!device.raw.online) {
      content = emptyState({ icon: "wifi-off", title: "设备离线", text: "设备注册上线后，通道会自动出现在这里。" });
    } else if (channels.length === 0) {
      content = emptyState({ icon: "clock", title: "未发现通道", text: "设备上线后会通过目录查询上报通道；若长时间为空，请检查设备配置。" });
    } else {
      const list = h("div", { class: "channel-list", role: "radiogroup", "aria-label": "通道", onkeydown: event => moveSelection(event, channels) },
        channels.map((channel, index) => channelRow(device, channel, index, channels)));
      content = list;
    }
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("div", {}, h("h2", {}, "通道"), h("div", { class: "sub" }, "选择通道查看媒体状态；点播操作在每行右侧，打开详情不会自动点播"))),
      content);
  }

  function moveSelection(event, channels) {
    const step = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 }[event.key];
    if (!step) {
      return;
    }
    event.preventDefault();
    const index = Math.max(0, channels.findIndex(channel => channel.channel_id === channelID));
    channelID = channels[(index + step + channels.length) % channels.length].channel_id;
    update();
    left.querySelector(`[data-key="pick:${CSS.escape(channelID)}"]`)?.focus();
  }

  function channelRow(device, channel, index, channels) {
    const selected = channel.channel_id === channelID;
    const live = channel.live;
    const key = `channel:${channel.channel_id}`;
    const busy = pending.has(key);
    const liveBadge = channel.status !== "ON" ? badge("离线", "neutral")
      : live?.state === "streaming" ? badge("点播中", "success")
      : live ? badge({ preparing: "正在点播", inviting: "正在点播", stopping: "正在停止", cleanup_pending: "正在清理" }[live.state] || live.state, "pending")
      : badge("未点播", "neutral", false);
    const label = channel.name || channel.channel_id;
    return h("div", { class: `channel${selected ? " is-selected" : ""}` },
      h("button", {
        type: "button", role: "radio", class: "channel-pick", "aria-checked": String(selected),
        tabindex: selected || (!channels.some(item => item.channel_id === channelID) && index === 0) ? "0" : "-1",
        dataset: { key: `pick:${channel.channel_id}` }, onclick: () => { channelID = channel.channel_id; update(); },
      }, h("span", { class: "radio", "aria-hidden": "true" }), h("span", { class: "cell-title" }, h("strong", {}, label), h("span", { class: "mono" }, channel.channel_id)), liveBadge),
      h("div", { class: "channel-ops" },
        channel.status === "ON" && device.raw.online && !live ? h("button", {
          type: "button", class: "btn xs", disabled: busy, "aria-busy": String(busy), "aria-label": `开始点播 ${label}`, dataset: { key: `live:${channel.channel_id}` },
          onclick: () => { channelID = channel.channel_id; void startLive(channel); },
        }, busy ? null : icon("play", "sm"), "点播") : null,
        live?.state === "streaming" ? h("button", {
          type: "button", class: "btn xs danger-subtle", disabled: busy, "aria-label": `停止点播 ${label}`, dataset: { key: `stop:${channel.channel_id}` },
          onclick: () => stopLive(channel),
        }, icon("square", "sm"), "停止") : null));
  }

  // 点播接口在 live 就绪后同时返回一组播放地址。
  function startLive(channel) {
    return run(`channel:${channel.channel_id}`, () => api.playChannel(id, channel.channel_id), `${channel.name || channel.channel_id} 已开始点播`);
  }

  function stopLive(channel) {
    confirmAction({
      title: "停止点播", message: `停止「${channel.name || channel.channel_id}」的点播后，该通道所有观看者（包括其他用户）都会断开，已签发的播放地址随之失效。`, confirmLabel: "停止点播",
      async action() {
        await api.stopLive(channel.live.live_id);
        toast("已停止点播", "success");
        void refresh();
      },
    });
  }

  // 推流地址只在签发响应里出现；离开详情后无法再取回，需要时停止后重新签发。
  function publishCard(device) {
    const raw = device.raw;
    const busy = pending.has("push");
    const issued = publish && raw.state === "issued" ? publish : null;
    const actions = [];
    if (raw.state === "idle") {
      actions.push(h("button", { class: "btn sm primary", type: "button", disabled: busy, "aria-busy": String(busy), dataset: { key: "publish" },
        onclick: async () => {
          const result = await run("push", () => api.publishPushDevice(id), "已签发推流地址");
          if (result) {
            publish = result;
            update();
          }
        } }, busy ? null : icon("link", "sm"), "签发推流地址"));
    } else {
      actions.push(h("button", { class: "btn sm danger-subtle", type: "button", disabled: busy, dataset: { key: "push-stop" }, onclick: () => stopPush(device) },
        icon("square", "sm"), raw.state === "issued" ? "撤销签发" : "停止推流"));
    }
    let body;
    if (issued) {
      body = [h("div", { class: "url-rows bordered" }, [["RTMP", issued.rtmp_url], ["RTSP", issued.rtsp_url], ["WHIP", issued.whip_url]].map(([label, url]) =>
        h("div", { class: "url-row" }, h("div", { class: "url-proto" }, h("strong", {}, label)),
          h("div", { class: "url-side" }, copyButton(url, `${label} 推流地址`)), h("div", { class: "url-value" }, url)))),
      h("ul", { class: "rules" },
        h("li", {}, `三个地址共用一个一次性授权，请按设备支持的协议选择其一；${new Date(issued.expires_at).toLocaleTimeString("zh-CN", { hour12: false })} 前首次连接有效。`),
        h("li", {}, "地址只在本次签发时显示，离开详情后无法再次查看；如未保存，请撤销后重新签发。"))];
    } else if (raw.state === "issued") {
      body = h("p", { class: "muted" }, "已签发推流地址，等待设备连接。地址只在签发时显示；如未保存，请撤销签发后重新签发。");
    } else if (raw.state === "verified") {
      body = h("p", { class: "muted" }, "设备已使用推流地址连接并正在推流。停止后需要重新签发地址才能再次推流。");
    } else {
      body = h("p", { class: "muted" }, "尚未签发推流地址。签发后同时提供 RTMP、RTSP、WHIP 三种地址，共用一个一次性授权。");
    }
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("div", {}, h("h2", {}, "推流地址"), h("div", { class: "sub" }, "临时签发 · 一次性授权")), h("div", { class: "head-actions" }, actions)),
      h("div", { class: "card-body stack" }, body));
  }

  function stopPush(device) {
    const issued = device.raw.state === "issued";
    confirmAction({
      title: issued ? "撤销签发" : "停止推流",
      message: issued ? "撤销后已签发的推流地址立即失效。" : `停止「${device.name}」的推流后，正在观看的用户会断开；再次推流需要重新签发地址。`,
      confirmLabel: issued ? "撤销签发" : "停止推流",
      async action() {
        await api.stopPushDevice(id);
        publish = null;
        toast(issued ? "已撤销签发" : "已停止推流", "success");
        void refresh();
      },
    });
  }

  update();
  return { update, unmount: () => playback.destroy() };
}
