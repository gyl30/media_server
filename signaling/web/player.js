import { WHEPPreview } from "/whep.js";
import { store, deviceHash } from "/store.js";
import { h, icon, protoTag, errorMessage } from "/ui.js";

export const PLAYER_STATE = { preparing: "正在准备", negotiating: "正在建立 WHEP 连接", streaming: "播放中", stopping: "正在关闭", failed: "播放失败", ended: "已结束" };

export function createPlayer(item, onUpdate) {
  const player = { ...item, state: "preparing", error: "", gesture: false, streamingAt: 0 };
  player.video = h("video", { autoplay: true, playsinline: true, muted: true });
  player.video.muted = true;
  player.top = h("div", { class: "tile-top" });
  player.center = h("div", { class: "tile-center" });
  player.bottom = h("div", { class: "tile-bottom" });
  player.element = h("div", { class: "tile", role: "group" }, player.video, player.top, player.center, player.bottom);
  player.preview = new WHEPPreview(player.video, update => {
    player.state = update.state;
    player.error = update.error;
    player.gesture = update.needsPlaybackGesture;
    if (update.state === "streaming" && !player.streamingAt) player.streamingAt = Date.now();
    onUpdate(player);
  });
  player.video.addEventListener("playing", () => {
    player.gesture = false;
    onUpdate(player);
  });
  return player;
}

export function startPlayer(player) {
  player.streamingAt = 0;
  void player.preview.start(player.target, player.label).catch(() => {});
}

export function renderPlayer(player, { slot = null, active = false, detailLink = false, onClose }) {
  const stop = handler => event => { event.stopPropagation(); handler(); };
  const failed = player.state === "failed" || player.state === "ended";
  const label = PLAYER_STATE[player.state] || player.state;
  player.element.className = `tile is-${player.state}${active ? " is-active" : ""}`;
  player.element.setAttribute("aria-label", `${slot === null ? "" : `画面 ${slot}：`}${player.label}，${label}`);
  player.top.replaceChildren(h("span", { class: `live-dot${player.state === "streaming" ? " on" : failed ? " err" : ""}` }),
    h("span", { class: "name", title: player.label }, player.label), protoTag(player.protocol),
    h("span", { class: "tile-state" }, label),
    h("button", { type: "button", class: "tile-btn", "aria-label": "关闭画面", title: "关闭画面（只释放本画面）", onclick: stop(onClose) }, icon("x", "sm")));
  if (failed) {
    player.center.replaceChildren(icon("alert", "lg"), h("span", {}, errorMessage({ code: player.error })), h("p", {}, "不会自动重连。可以重试，或关闭该画面。"),
      h("div", { class: "inline" },
        h("button", { type: "button", class: "btn sm", onclick: stop(() => startPlayer(player)) }, icon("refresh", "sm"), "重试"),
        h("button", { type: "button", class: "btn sm", onclick: stop(onClose) }, "关闭")));
  } else if (player.state === "streaming" && player.gesture) {
    player.center.replaceChildren(h("button", { type: "button", class: "btn primary", onclick: stop(() => void player.video.play().catch(() => {})) }, icon("play", "sm"), "点击播放"));
  } else if (player.state !== "streaming") {
    player.center.replaceChildren(h("span", { class: "spinner", "aria-hidden": "true" }), h("span", {}, `${label}…`));
  } else {
    player.center.replaceChildren();
  }
  const fullscreen = document.fullscreenElement === player.element;
  player.bottom.replaceChildren(slot === null ? h("span") : h("span", { class: "slot-number" }, String(slot).padStart(2, "0")), h("span", { class: "meta" }),
    h("button", { type: "button", class: "tile-btn", "aria-label": player.video.muted ? "打开声音" : "静音", title: player.video.muted ? "打开声音" : "静音",
      disabled: player.state !== "streaming", onclick: stop(() => { player.video.muted = !player.video.muted; renderPlayer(player, { slot, active, detailLink, onClose }); }) },
    icon(player.video.muted ? "volume-off" : "volume", "sm")),
    detailLink ? h("a", { class: "tile-btn", href: deviceHash(player.kind, player.deviceID), "aria-label": "设备详情", title: "设备详情（离开实时监控，关闭全部画面）",
      onclick: event => event.stopPropagation() }, icon("info", "sm")) : null,
    h("button", { type: "button", class: "tile-btn", "aria-label": fullscreen ? "退出全屏" : "全屏", title: fullscreen ? "退出全屏（Esc）" : "全屏",
      onclick: stop(() => (document.fullscreenElement ? document.exitFullscreen() : player.element.requestFullscreen()).catch(() => {})) },
    icon(fullscreen ? "minimize" : "maximize", "sm")));
}

function currentStream(target) {
  if (target.source_id) {
    const session = store.sources.find(source => source.source_id === target.source_id)?.session;
    return { id: session?.state === "created" && session.media_ready ? session.stream_id : "", at: store.updatedAt };
  }
  if (target.push_device_id) {
    const device = store.pushDevices.find(item => item.device_id === target.push_device_id);
    return { id: device?.state === "publishing" ? device.token : "", at: store.updatedAt };
  }
  const cache = store.channels.get(target.device_id);
  const channel = (cache?.channels || []).find(item => item.channel_id === target.channel_id);
  return { id: channel?.live?.state === "streaming" ? channel.live.live_id : "", at: cache?.at ?? 0 };
}

export function endIfStreamChanged(player) {
  const session = player.preview.current;
  const stream = currentStream(player.target);
  if (session?.streamID && player.streamingAt && stream.at > player.streamingAt && stream.id !== session.streamID) {
    void player.preview.end(session);
  }
}
