import { api } from "/api.js";
import { store, unifiedDevices, refresh, refreshing, syncChannels, loadChannels, pushURLs, sourceReady } from "/store.js";
import { h, icon, badge, emptyState, toast, confirmAction, errorMessage, formatAgo, replaceKeepingFocus } from "/ui.js";
import { openDeviceDialog, deleteDevice, startSource, stopSource, stopPush, renewPushURL, pushURLBox, playItem } from "/devices.js";
import { createPlayer, renderPlayer, startPlayer, endIfStreamChanged } from "/player.js";
import { TalkCapture } from "/talk.js";

const LIVE_LABEL = { preparing: "正在点播", inviting: "正在点播", stopping: "正在停止" };
const TALK_ACTION = { stopped: "开始对讲", preparing: "取消对讲", transmitting: "停止对讲", stopping: "正在停止对讲" };
const TALK_STATE = { preparing: "正在连接", transmitting: "正在上送麦克风音频" };

export function mountDetail(root, ctx, kind, id) {
  let mounted = true;
  let channelID = "";
  let player = null;
  let talkState = "stopped";
  let talkError = "";
  const talk = new TalkCapture((state, error = "") => {
    talkState = state;
    talkError = error;
    if (mounted) update();
  });
  const head = h("div", { class: "detail-head" });
  const left = h("div", { class: "detail-col" });
  const playbackHead = h("div", { class: "card-head" });
  const playbackBody = h("div", { class: "player-slot" });
  const page = h("div", { class: "page" }, head, h("div", { class: "detail-grid" }, left,
    h("section", { class: "card detail-player", "aria-label": "播放" }, playbackHead, playbackBody)));
  root.replaceChildren(page);

  const find = () => unifiedDevices().find(device => device.kind === kind && device.id === id);
  const mountedAt = Date.now();
  const onClose = () => {
    closePlayer();
    channelID = "";
    update();
  };

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
      root.replaceChildren(h("div", { class: "page" }, h("section", { class: "card" }, confirmed
        ? emptyState({ icon: "search", title: "设备不存在或已删除", action: h("a", { class: "btn", href: "#devices" }, "返回设备管理") })
        : emptyState({ icon: "clock", title: "正在加载…" }))));
      return;
    }
    if (!root.contains(page)) {
      root.replaceChildren(page);
    }
    ctx.setTitle(device.name);
    if (kind === "gb" && !device.raw.online) void talk.stop("设备已离线");
    if (kind === "gb" && (device.raw.online || player)) {
      syncChannels([id]);
    }
    replaceKeepingFocus(head, headContent(device));
    replaceKeepingFocus(left, [
      kind === "push" ? publishCard(device) : infoCard(device),
      kind === "gb" ? channelCard(device) : null,
    ].filter(Boolean));
    renderPlayback(device);
  }

  function headContent(device) {
    const source = device.raw;
    const actions = [];
    if (kind === "gb") {
      const state = talkState === "stopped" ? talkError : TALK_STATE[talkState];
      if (state) {
        actions.push(h("span", { class: `talk-state${talkState === "stopped" ? " danger-text" : ""}`, role: "status" },
          talkState === "stopped" ? icon("alert", "sm") : null, state));
      }
      actions.push(h("button", { class: "btn", type: "button", dataset: { key: "talk" },
        disabled: talkState === "stopping" || (talkState === "stopped" && !source.online),
        onclick: () => talkState === "stopped" ? talk.start(id) : void talk.stop() }, TALK_ACTION[talkState]));
    }
    if (kind === "rtsp") {
      actions.push(source.session
        ? h("button", { class: "btn", type: "button", dataset: { key: "stop" }, onclick: () => stopSource(device) }, "停止拉流")
        : h("button", { class: "btn", type: "button", dataset: { key: "start" }, onclick: () => void startSource(device) },
          source.desired_state === "running" ? "重新连接" : "开始拉流"));
    }
    if (kind !== "gb") {
      actions.push(h("button", { class: "btn", type: "button", dataset: { key: "edit" }, onclick: () => openDeviceDialog(device) }, "编辑"));
    }
    actions.push(h("button", { class: "btn danger", type: "button", dataset: { key: "delete" }, onclick: () => deleteDevice(device, ctx) }, "删除"));
    return [
      h("div", { class: "detail-meta" }, h("span", {}, device.type), badge(device.status.label, device.status.tone),
        device.status.detail ? h("span", { class: "muted" }, device.status.detail) : null),
      h("div", { class: "head-actions" }, actions),
    ];
  }

  function infoCard(device) {
    const raw = device.raw;
    const fields = [];
    if (kind === "gb") {
      const channels = device.channels || [];
      fields.push(["国标编号", h("span", { class: "mono" }, device.id)],
        ["最近心跳", raw.last_seen ? formatAgo(Date.parse(raw.last_seen)) : "—"],
        ["通道", raw.online ? (device.channels ? `${channels.length} 个，在线 ${channels.filter(channel => channel.status === "ON").length} 个` : "正在获取…") : "—"]);
    } else {
      fields.push(["来源地址", h("span", { class: "mono" }, raw.url)]);
      if (device.protocol === "WHEP") {
        fields.push(["Bearer Token", raw.password_set ? "已设置" : "未设置"]);
      } else if (device.protocol === "RTSP") {
        fields.push(["用户名", raw.username || "未设置"]);
        if (raw.username) fields.push(["密码", raw.password_set ? "已设置" : "未设置"]);
      }
    }
    return h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "设备信息")),
      h("dl", { class: "kv" }, fields.map(([label, value]) => h("div", {}, h("dt", {}, label), h("dd", {}, value)))));
  }

  function channelCard(device) {
    const channels = device.channels || [];
    let content;
    if (!device.raw.online) {
      content = emptyState({ icon: "camera", title: "设备离线" });
    } else if (!device.channels) {
      content = device.channelError
        ? emptyState({ icon: "alert", tone: "danger", title: "通道加载失败", text: errorMessage(device.channelError) })
        : emptyState({ icon: "clock", title: "正在获取通道…" });
    } else if (channels.length === 0) {
      content = emptyState({ icon: "camera", title: "设备未上报通道" });
    } else {
      content = h("ul", { class: "channel-list" }, channels.map(channel => channelRow(device, channel)));
    }
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("h2", {}, "通道"), device.raw.online && device.channels ? h("span", { class: "muted" }, String(channels.length)) : null),
      content);
  }

  function channelRow(device, channel) {
    const live = channel.live;
    const label = channel.name || channel.channel_id;
    const current = player?.key === `gb:${id}:${channel.channel_id}`;
    const state = channel.status !== "ON" ? badge("离线", "neutral")
      : live?.media_ready ? badge("点播中", "success")
      : live?.state === "streaming" ? badge("等待媒体", "pending")
      : live ? badge(LIVE_LABEL[live.state] || live.state, "pending")
      : null;
    return h("li", { class: `channel${current ? " is-current" : ""}` },
      h("button", {
        type: "button", class: "channel-main", disabled: channel.status !== "ON" || !device.raw.online, "aria-current": current ? "true" : undefined,
        dataset: { key: `play:${channel.channel_id}` }, onclick: () => playChannel(device, channel),
      }, icon(current ? "video" : "play", "sm"), h("span", { class: "channel-text" }, h("strong", {}, label), h("span", { class: "mono muted" }, channel.channel_id)), state),
      live?.state === "streaming" ? h("button", {
        type: "button", class: "btn sm danger", "aria-label": `停止点播 ${label}`, dataset: { key: `stop:${channel.channel_id}` },
        onclick: () => stopLive(channel),
      }, "停止点播") : null);
  }

  function playChannel(device, channel) {
    if (player?.key === `gb:${id}:${channel.channel_id}`) {
      return;
    }
    channelID = channel.channel_id;
    play(playItem(device, channel));
    update();
  }

  function stopLive(channel) {
    confirmAction({
      title: "停止点播", message: `停止「${channel.name || channel.channel_id}」的点播后，所有观看者都会断开。`, confirmLabel: "停止点播",
      async action() {
        await api.stopLive(channel.live.live_id);
        void loadChannels([id]);
      },
    });
  }

  function publishCard(device) {
    const raw = device.raw;
    const action = raw.state === "publishing"
      ? h("button", { class: "btn sm danger", type: "button", dataset: { key: "push-stop" }, onclick: () => stopPush(device) }, "停止推流")
      : raw.state === "pending" ? null
      : h("button", { class: "btn sm", type: "button", dataset: { key: "push-renew" }, onclick: () => void renewPushURL(device) }, icon("refresh", "sm"), "重新生成地址");
    return h("section", { class: "card" },
      h("div", { class: "card-head" }, h("h2", {}, "推流地址"), action),
      h("div", { class: "card-body stack" }, pushURLBox(pushURLs(raw)[0][2]), raw.state === "pending" ? h("p", { class: "muted" }, "首次接入后此地址失效") : null));
  }

  function closePlayer() {
    const closing = player;
    player = null;
    if (closing) void closing.preview.closeViewer().then(error => {
      if (error) toast(errorMessage({ code: "whep_delete_500" }), "danger");
    });
  }

  function play(item) {
    closePlayer();
    const next = createPlayer(item, () => {
      if (player === next) renderPlayer(next, { onClose });
    });
    player = next;
    startPlayer(next);
  }

  function currentItem(device) {
    if (kind === "gb") {
      const channel = (device.channels || []).find(item => item.channel_id === channelID);
      return device.raw.online && channel?.status === "ON" ? playItem(device, channel) : null;
    }
    return (kind === "rtsp" ? sourceReady(device.raw) : device.raw.media_ready) ? playItem(device) : null;
  }

  function idleState(device, item) {
    if (item) {
      return emptyState({ action: h("button", { class: "btn primary", type: "button", dataset: { key: "detail-play" },
        onclick: () => { play(item); update(); } }, icon("play", "sm"), "播放") });
    }
    if (kind !== "gb") {
      return emptyState({ icon: "video", title: device.status.label });
    }
    return emptyState({ icon: "video", title: !device.raw.online ? "设备离线" : channelID ? "通道离线" : "选择通道播放" });
  }

  function renderPlayback(device) {
    const item = currentItem(device);
    if (player && kind === "gb" && player.key !== `gb:${id}:${channelID}`) closePlayer();
    if (player) {
      if (item) player.target = item.target;
      endIfStreamChanged(player);
    }
    replaceKeepingFocus(playbackHead, [h("h2", {}, kind === "gb" && item ? item.label : "播放")]);
    if (player) {
      renderPlayer(player, { onClose });
      if (playbackBody.firstChild !== player.element) playbackBody.replaceChildren(player.element);
    } else {
      replaceKeepingFocus(playbackBody, [idleState(device, item)]);
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
  const onFullscreen = () => { if (player) renderPlayer(player, { onClose }); };
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
