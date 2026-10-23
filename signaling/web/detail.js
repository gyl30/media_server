import { api } from "/api.js";
import { store, unifiedDevices, refresh, refreshing, syncChannels, loadChannels, pushURLs, formatClock, sourceReady, PUSH_PROTOCOLS } from "/store.js";
import {
  h, icon, badge, protoTag, emptyState, toast, confirmAction, errorMessage, formatAgo, replaceKeepingFocus,
} from "/ui.js";
import {
  openDeviceDialog, deleteDevice, editState, startSource, stopSource, stopPush, renewPushURL, pushURLRows, playItem,
} from "/devices.js";
import { createPlayer, renderPlayer, startPlayer, endIfStreamChanged } from "/player.js";
import { TalkCapture } from "/talk.js";

const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };
const LIVE_LABEL = { preparing: "正在点播", inviting: "正在点播", stopping: "正在停止" };

export function mountDetail(root, ctx, kind, id) {
  let mounted = true;
  const pending = new Set();
  let channelID = "";
  const head = h("div", { class: "detail-head" });
  const left = h("div", { class: "detail-col" });
  const right = h("div", { class: "detail-col" });
  let player = null;
  let talkState = "stopped";
  let talkError = "";
  const talk = new TalkCapture((state, error = "") => {
    talkState = state;
    talkError = error;
    if (mounted) update();
  });
  const playbackHead = h("div", { class: "card-head" });
  const playbackBody = h("div", { class: "card-body" });
  right.append(h("section", { class: "card detail-player", "aria-label": "视频播放" }, playbackHead, playbackBody));
  const page = h("div", { class: "page" }, head, h("div", { class: "detail-grid" }, left, right));
  root.replaceChildren(page);

  const find = () => unifiedDevices().find(device => device.kind === kind && device.id === id);
  const mountedAt = Date.now();

  function update() {
    if (!mounted) return;
    const device = find();
    if (!device) {
      void talk.stop();
      closePlayer();
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
    if (kind === "gb" && !device.raw.online) void talk.stop("设备已离线");
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
    renderPlayback(device);
  }

  function headContent(device) {
    const editable = editState(device);
    const source = device.raw;
    return [
      h("div", { class: "detail-title" }, h("span", { class: "detail-symbol" }, icon(KIND_ICON[kind], "lg")),
        h("div", {}, h("h1", {}, device.name || "未命名"), h("div", { class: "line" },
          protoTag(device.protocol), h("span", {}, device.mode), badge(device.status.label, device.status.tone)))),
      h("div", { class: "head-actions" },
        kind === "gb" ? h("button", { class: "btn", type: "button", dataset: { key: "talk" },
          disabled: talkState === "stopping" || (talkState === "stopped" && !source.online),
          onclick: () => talkState === "stopped" ? talk.start(id) : void talk.stop(),
          title: "向该设备上送麦克风；设备回传声音仍由视频播放器独立开启" },
        { stopped: "开始对讲", preparing: "取消对讲", transmitting: "停止对讲", stopping: "正在停止对讲…" }[talkState]) : null,
        kind === "rtsp" && !source.session
          ? h("button", { class: "btn primary", type: "button", dataset: { key: "start" }, onclick: () => void startSource(device) },
            icon("refresh", "sm"), "重试连接") : null,
        kind === "rtsp" && source.session
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
        ["对讲上传", talkError || { stopped: "未启动", preparing: "正在准备麦克风和设备连接", transmitting: "正在上送麦克风，设备听音未确认", stopping: "正在关闭采集和本次会话" }[talkState], true],
        ["最近心跳", raw.last_seen ? formatAgo(Date.parse(raw.last_seen)) : "—"],
        ["通道数量", raw.online ? (device.channels ? `${channels.length} 个，在线 ${channels.filter(channel => channel.status === "ON").length} 个` : "正在获取…") : "设备不在线"]);
    } else if (kind === "rtsp") {
      fields.push(["拉流状态", `${device.status.label}${device.status.detail ? ` · ${device.status.detail}` : ""}`], ["接入意图", raw.desired_state === "running" ? "自动拉流" : "已停止"],
        ["来源地址", h("span", { class: "mono" }, raw.url), true],
        ["认证", raw.username ? `用户名 ${raw.username} · 密码不回显` : raw.password_set ? "Bearer Token 已设置 · 不回显" : "未设置"], ["当前媒体流", raw.session ? h("span", { class: "mono" }, raw.session.stream_id) : "—"],
        ["资源标识", h("span", { class: "mono" }, device.id), true]);
    } else {
      fields.push(["接入协议", PUSH_PROTOCOLS[raw.protocol]], ["推流状态", device.status.label],
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
        h("div", { class: "sub" }, "选择通道后点击播放；打开详情不会自动点播"))),
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
      : live?.media_ready ? badge("点播中", "success")
      : live?.state === "streaming" ? badge("等待媒体", "pending")
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
    const raw = device.raw;
    const publishing = raw.state === "publishing";
    const actions = [publishing
      ? h("button", { class: "btn sm danger-subtle", type: "button", dataset: { key: "push-stop" }, onclick: () => stopPush(device) }, icon("square", "sm"), "停止推流")
      : null,
    raw.state === "pending" ? null : h("button", {
      class: "btn sm", type: "button", disabled: publishing, title: publishing ? "正在推流，停止推流后才能重新生成地址" : "",
      dataset: { key: "push-renew" }, onclick: () => void renewPushURL(device),
    }, icon("refresh", "sm"), "重新生成地址")].filter(Boolean);
    const text = {
      pending: `地址 ${formatClock(raw.expires_at)} 前首次接入有效，首次接入成功后立即失效。`,
      used: `地址已于 ${formatClock(raw.used_at)} 被使用，当前没有推流。需要再次接入时重新生成地址。`,
      expired: `地址已于 ${formatClock(raw.expires_at)} 过期且未被使用。重新生成后 5 分钟内有效。`,
      publishing: `地址已于 ${formatClock(raw.used_at)} 被使用，推流正在进行。推流认证不代表音视频一定正常，请以播放画面为准。`,
    }[raw.state];
    const protocolNote = `按设备协议显示 ${PUSH_PROTOCOLS[raw.protocol]} 地址；服务端当前不限制推流协议。`;
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("div", {}, h("h2", {}, "接入地址"), h("div", { class: "sub" }, `一次性 · ${device.status.label}`)), h("div", { class: "head-actions" }, actions)),
      h("div", { class: "card-body stack" },
        pushURLRows(pushURLs(raw)),
        h("div", { class: "hint" }, text),
        h("p", { class: "muted tiny" }, protocolNote)));
  }

  function closePlayer() {
    const closing = player;
    player = null;
    if (closing) void closing.preview.closeViewer().then(error => {
      if (error) toast(errorMessage({ code: "whep_delete_500" }), "danger");
    });
  }

  function renderPlayback(device) {
    const channel = (device.channels || []).find(item => item.channel_id === channelID);
    const item = kind === "gb" ? device.raw.online && channel?.status === "ON" ? playItem(device, channel) : null
      : (kind === "rtsp" ? sourceReady(device.raw) : device.raw.media_ready) ? playItem(device) : null;
    if (player && kind === "gb" && player.key !== `gb:${id}:${channelID}`) closePlayer();
    if (player) {
      if (item) player.target = item.target;
      endIfStreamChanged(player);
    }
    replaceKeepingFocus(playbackHead, [h("div", {}, h("h2", {}, "视频播放"), h("div", { class: "sub" }, channel?.name || device.name)),
      h("button", { class: "btn sm primary", type: "button", disabled: !item || Boolean(player), dataset: { key: "detail-play" },
        onclick: () => {
          const next = createPlayer(item, () => {
            if (player === next) renderPlayer(next, { onClose: () => { closePlayer(); update(); } });
          });
          player = next;
          renderPlayback(device);
          startPlayer(next);
        } }, icon("play", "sm"), "播放")]);
    if (player) {
      renderPlayer(player, { onClose: () => { closePlayer(); update(); } });
      if (playbackBody.firstChild !== player.element) playbackBody.replaceChildren(player.element);
    } else {
      playbackBody.replaceChildren(emptyState({ icon: item ? "play" : "film", title: item ? "点击播放预览视频" : "媒体未就绪",
        text: item ? "本页播放使用独立 WHEP 连接；关闭画面不停止设备点播或拉流。"
          : kind === "push" ? "设备未推流。使用接入地址连接后可以播放。"
          : kind === "gb" ? "设备和通道在线后可点击播放，按需点播；打开详情不会自动点播。"
          : "上游媒体就绪后可以播放；连接失败时检查来源地址，或重试连接。" }));
    }
  }

  const onPageHide = () => {
    void talk.stop();
    const closing = player;
    player = null;
    closing?.preview.closeForPageHide();
    const device = find();
    if (device) renderPlayback(device);
  };
  const onFullscreen = () => { if (player) renderPlayer(player, { onClose: () => { closePlayer(); update(); } }); };
  const onVisibility = () => { if (document.hidden) void talk.stop(); };
  window.addEventListener("pagehide", onPageHide);
  document.addEventListener("visibilitychange", onVisibility);
  document.addEventListener("fullscreenchange", onFullscreen);

  update();
  return { update, unmount() {
    mounted = false;
    window.removeEventListener("pagehide", onPageHide);
    document.removeEventListener("visibilitychange", onVisibility);
    document.removeEventListener("fullscreenchange", onFullscreen);
    void talk.stop();
    closePlayer();
    if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
  } };
}
