// 设备详情：按 GB28181、RTSP 拉流、推流设备分别展示信息与操作，共用页面布局与播放地址面板。
import { api } from "/api.js";
import { store, unifiedDevices, refresh, refreshing, syncChannels, loadChannels, PUSH_PROTOCOLS } from "/store.js";
import {
  h, icon, badge, protoTag, emptyState, toast, confirmAction, errorMessage, formatAgo, replaceKeepingFocus,
} from "/ui.js";
import {
  openDeviceDialog, deleteDevice, editState, startSource, stopSource, stopPush, issuePublish, playItem,
} from "/devices.js";
import { createPlaybackPanel } from "/playback.js";

const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };
const LIVE_LABEL = { preparing: "正在点播", inviting: "正在点播", stopping: "正在停止", cleanup_pending: "正在清理" };

export function mountDetail(root, ctx, kind, id) {
  const pending = new Set();
  let channelID = "";
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
    if (kind === "gb" && device.raw.online) {
      syncChannels([id]);
    }
    const channels = device.channels || [];
    if (kind === "gb" && !channels.some(channel => channel.channel_id === channelID)) {
      channelID = (channels.find(channel => channel.live?.state === "streaming") || channels.find(channel => channel.status === "ON") || channels[0])?.channel_id || "";
    }
    replaceKeepingFocus(head, headContent(device));
    replaceKeepingFocus(left, [infoCard(device),
      kind === "gb" ? channelCard(device) : null,
      kind === "push" ? publishCard(device) : null].filter(Boolean));
    playback.update(playbackState(device));
  }

  function headContent(device) {
    const editable = editState(device);
    const source = device.raw;
    return [
      h("div", { class: "detail-title" }, h("span", { class: "detail-symbol" }, icon(KIND_ICON[kind], "lg")),
        h("div", {}, h("h1", {}, device.name || "未命名"), h("div", { class: "line" },
          protoTag(device.protocol), h("span", {}, device.mode), badge(device.status.label, device.status.tone)))),
      h("div", { class: "head-actions" },
        kind === "rtsp" && !source.session
          ? h("button", { class: "btn primary", type: "button", dataset: { key: "start" }, onclick: () => void startSource(device) },
            icon("play", "sm"), source.desired_state === "running" ? "重新拉流" : "开始拉流") : null,
        kind === "rtsp" && (source.session || source.desired_state === "running")
          ? h("button", { class: "btn danger-subtle", type: "button", dataset: { key: "stop" }, onclick: () => stopSource(device) }, icon("square", "sm"), "停止拉流") : null,
        kind !== "gb" ? h("button", { class: "btn", type: "button", disabled: !editable.ok, title: editable.reason, dataset: { key: "edit" },
          onclick: () => openDeviceDialog(device) }, icon("edit", "sm"), "编辑") : null,
        h("button", { class: "btn danger-subtle", type: "button", dataset: { key: "delete" }, onclick: () => deleteDevice(device, ctx) }, icon("trash", "sm"), "删除")),
    ];
  }

  function infoCard(device) {
    const raw = device.raw;
    const fields = [];
    if (kind === "gb") {
      const channels = device.channels || [];
      fields.push(["国标设备编号", h("span", { class: "mono" }, device.id)], ["注册状态", device.status.label],
        ["最近心跳", raw.last_seen ? formatAgo(Date.parse(raw.last_seen)) : "—"],
        ["通道数量", raw.online ? (device.channels ? `${channels.length} 个，在线 ${channels.filter(channel => channel.status === "ON").length} 个` : "正在获取…") : "设备不在线"]);
    } else if (kind === "rtsp") {
      fields.push(["拉流状态", `${device.status.label}${device.status.detail ? ` · ${device.status.detail}` : ""}`], ["期望状态", raw.desired_state === "running" ? "拉流" : "停止"],
        ["来源地址", h("span", { class: "mono" }, raw.url), true],
        ["认证", raw.username ? `用户名 ${raw.username} · 密码不回显` : "未设置"], ["当前媒体流", raw.session ? h("span", { class: "mono" }, raw.session.stream_id) : "—"],
        ["资源标识", h("span", { class: "mono" }, device.id), true]);
    } else {
      fields.push(["接入协议", PUSH_PROTOCOLS[raw.protocol] || "未指定"], ["推流状态", device.status.label],
        ["状态说明", device.status.detail || "—", true], ["资源标识", h("span", { class: "mono" }, device.id), true]);
    }
    return h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "设备信息")),
      h("div", { class: "card-body" }, h("dl", { class: "kv" }, fields.map(([label, value, wide]) =>
        h("div", { class: wide ? "wide" : "" }, h("dt", {}, label), h("dd", {}, value))))));
  }

  function channelCard(device) {
    const channels = device.channels || [];
    let content;
    if (!device.raw.online) {
      content = emptyState({ icon: "wifi-off", title: "设备离线", text: "设备注册上线后，通道会自动出现在这里。" });
    } else if (!device.channels) {
      content = device.channelError
        ? emptyState({ icon: "alert", tone: "danger", title: "通道加载失败", text: `${errorMessage(device.channelError)}。下一次刷新时会重新获取。` })
        : emptyState({ icon: "clock", title: "正在获取通道…" });
    } else if (channels.length === 0) {
      content = emptyState({ icon: "layers", title: "未发现通道", text: "设备上线后会通过目录查询上报通道；若长时间为空，请检查设备配置。" });
    } else {
      content = h("div", { class: "channel-list", role: "radiogroup", "aria-label": "通道", onkeydown: event => moveSelection(event, channels) },
        channels.map((channel, index) => channelRow(device, channel, index, channels)));
    }
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("div", {}, h("h2", {}, `通道列表${device.channels ? ` · ${channels.length}` : ""}`),
        h("div", { class: "sub" }, "选择通道查看播放地址；打开详情不会自动点播"))),
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
    const busy = pending.has(channel.channel_id);
    const label = channel.name || channel.channel_id;
    const state = channel.status !== "ON" ? badge("离线", "neutral")
      : live?.state === "streaming" ? badge("点播中", "success")
      : live ? badge(LIVE_LABEL[live.state] || live.state, "pending")
      : badge("未点播", "neutral", false);
    return h("div", { class: `channel${selected ? " is-selected" : ""}` },
      h("button", {
        type: "button", role: "radio", class: "channel-pick", "aria-checked": String(selected),
        tabindex: selected || (!channels.some(item => item.channel_id === channelID) && index === 0) ? "0" : "-1",
        dataset: { key: `pick:${channel.channel_id}` }, onclick: () => { channelID = channel.channel_id; update(); },
      }, h("span", { class: "radio", "aria-hidden": "true" }), h("span", { class: "c-icon" }, icon("camera", "sm")),
      h("span", {}, h("strong", {}, label), h("span", { class: "mono" }, channel.channel_id)), state),
      h("div", { class: "channel-ops" },
        channel.status === "ON" && device.raw.online && !live ? h("button", {
          type: "button", class: "btn xs", disabled: busy, "aria-busy": String(busy), "aria-label": `开始点播 ${label}`, dataset: { key: `live:${channel.channel_id}` },
          onclick: () => { channelID = channel.channel_id; void startLive(channel); },
        }, busy ? null : icon("play", "sm"), "点播") : null,
        live?.state === "streaming" ? h("button", {
          type: "button", class: "btn xs danger-subtle", "aria-label": `停止点播 ${label}`, dataset: { key: `stop:${channel.channel_id}` },
          onclick: () => stopLive(channel),
        }, icon("square", "sm"), "停止") : null));
  }

  // 点播接口在 live 就绪后同时返回一组播放地址；界面以随后的通道快照为准。
  async function startLive(channel) {
    if (pending.has(channel.channel_id)) {
      return;
    }
    pending.add(channel.channel_id);
    update();
    try {
      await api.playChannel(id, channel.channel_id);
      toast(`${channel.name || channel.channel_id} 已开始点播`, "success");
    } catch (error) {
      toast(`点播失败：${errorMessage(error)}`, "danger");
    } finally {
      pending.delete(channel.channel_id);
      update();
      void loadChannels([id]);
    }
  }

  function stopLive(channel) {
    confirmAction({
      title: "停止点播", message: `停止「${channel.name || channel.channel_id}」的点播后，该通道所有观看者（包括其他用户）都会断开，已签发的播放地址随之失效。`, confirmLabel: "停止点播",
      async action() {
        await api.stopLive(channel.live.live_id);
        toast("已停止点播", "success");
        void loadChannels([id]);
      },
    });
  }

  function publishCard(device) {
    const state = device.raw.state;
    const action = state === "idle"
      ? h("button", { class: "btn sm primary", type: "button", dataset: { key: "publish" }, onclick: () => void issuePublish(device) }, icon("link", "sm"), "签发推流地址")
      : h("button", { class: "btn sm danger-subtle", type: "button", dataset: { key: "push-stop" }, onclick: () => stopPush(device) }, icon("square", "sm"), state === "issued" ? "撤销签发" : "停止推流");
    const text = {
      idle: "尚未签发推流地址。签发后服务端提供一次性推流地址，界面只显示设备所选协议的地址。",
      issued: "已签发推流地址，等待设备连接。地址只在签发时显示；如未保存，请撤销签发后重新签发。",
      verified: "设备已使用推流地址完成授权。授权验证不代表音视频一定正常，可在实时监控中预览确认。停止后需要重新签发地址才能再次推流。",
    }[state];
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("div", {}, h("h2", {}, "推流地址"), h("div", { class: "sub" }, "临时签发 · 一次性授权")), h("div", { class: "head-actions" }, action)),
      h("div", { class: "card-body" }, h("div", { class: "hint" }, text)));
  }

  // 只有可证明正在运行的媒体流才申请播放地址；否则给出该设备当前可做的下一步。
  function playbackState(device) {
    const raw = device.raw;
    if (kind === "gb") {
      const channel = (device.channels || []).find(item => item.channel_id === channelID);
      const title = channel ? `${device.name} · ${channel.name || channel.channel_id}` : device.name;
      const actions = playAction(raw.online && channel?.status === "ON" ? playItem(device, channel) : null);
      if (channel?.live?.state === "streaming") {
        return { title, actions, stream: { id: channel.live.live_id, source: { live_id: channel.live.live_id } } };
      }
      return { title, actions, notReady: gbNotReady(device, channel) };
    }
    if (kind === "rtsp") {
      if (raw.session?.state === "created") {
        return { title: device.name, actions: playAction(playItem(device)), stream: { id: raw.session.stream_id, source: { source_id: id } } };
      }
      const text = raw.session ? "拉流会话正在处理，就绪后会自动申请播放地址。"
        : raw.desired_state === "running" ? "已请求拉流，但当前没有运行中的会话。请检查来源地址与认证信息后重新拉流。"
        : "尚未开始拉流。开始拉流后才会生成播放地址。";
      return { title: device.name, actions: playAction(null), notReady: emptyState({ icon: raw.session ? "clock" : "film", title: "媒体未就绪", text }) };
    }
    if (raw.state === "verified") {
      return { title: device.name, actions: playAction(playItem(device)), stream: { id: raw.token, source: { push_device_id: id } } };
    }
    return { title: device.name, actions: playAction(null), notReady: emptyState({ icon: "film", title: "媒体未就绪 · 设备未推流", text: "设备使用推流地址连接后，这里才会生成播放地址。" }) };
  }

  // GB 通道在线即可预览（实时监控按现有流程点播）；RTSP / 推流需要运行中的媒体流。
  function playAction(item) {
    return [h("button", { class: "btn sm primary", type: "button", disabled: !item, dataset: { key: "play-center" },
      onclick: () => ctx.playInCenter(item) }, icon("play", "sm"), "实时监控预览")];
  }

  function gbNotReady(device, channel) {
    if (!device.raw.online) {
      return emptyState({ icon: "wifi-off", title: "媒体未就绪 · 设备离线", text: "设备注册上线后，可选择通道并开始点播。" });
    }
    if (!channel) {
      return emptyState({ icon: "layers", title: device.channels ? "没有可选通道" : "正在获取通道…" });
    }
    if (channel.status !== "ON") {
      return emptyState({ icon: "wifi-off", title: "媒体未就绪 · 通道离线", text: "该通道当前不可点播，请检查前端摄像机。" });
    }
    if (channel.live) {
      return emptyState({ icon: "clock", title: "点播进行中…", text: "媒体流就绪后会自动申请播放地址。" });
    }
    const busy = pending.has(channel.channel_id);
    return emptyState({
      icon: "film", title: "媒体未就绪", text: "该通道当前没有运行中的媒体流。打开详情不会自动点播，需要时请手动开始。",
      action: h("button", { class: "btn primary", type: "button", disabled: busy, "aria-busy": String(busy), dataset: { key: "start-live" },
        onclick: () => void startLive(channel) }, busy ? null : icon("play", "sm"), "开始点播"),
    });
  }

  update();
  return { update, unmount: () => playback.destroy() };
}
