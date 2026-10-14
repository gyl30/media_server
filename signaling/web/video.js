// 视频中心：设备树 + 单画面 / 四分屏。每个画面对象独占自己的 <video>、WHEPPreview 和 DOM 节点；
// 布局切换只移动画面节点，WHEPPreview 的状态回调绑定画面对象而不是槽位编号。
// 关闭画面只释放该画面的 WHEP 会话，不停止上游点播或取流。
import { WHEPPreview } from "/whep.js";
import { store, deviceHash } from "/store.js";
import { h, icon, protoTag, toast, errorMessage, replaceKeepingFocus } from "/ui.js";

const STATE_LABEL = { preparing: "正在准备", negotiating: "正在建立 WHEP 连接", streaming: "播放中", stopping: "正在关闭", failed: "播放失败", ended: "已结束" };

// 当前目标对应的运行中媒体流 ID；用于发现画面所播放的媒体流已结束或换代。
function currentStreamID(target) {
  if (target.source_id) {
    const session = store.sources.find(source => source.source_id === target.source_id)?.session;
    return session?.state === "created" ? session.stream_id : "";
  }
  if (target.push_device_id) {
    const device = store.pushDevices.find(item => item.device_id === target.push_device_id);
    return device?.state === "verified" ? device.token : "";
  }
  const channel = (store.channels.get(target.device_id) || []).find(item => item.channel_id === target.channel_id);
  return channel?.live?.state === "streaming" ? channel.live.live_id : "";
}

function createTile(item, video) {
  const tile = { ...item, state: "preparing", error: "", gesture: false, streamingAt: 0 };
  tile.video = h("video", { autoplay: true, playsinline: true, muted: true });
  tile.video.muted = true;
  tile.top = h("div", { class: "tile-top" });
  tile.center = h("div", { class: "tile-center" });
  tile.bottom = h("div", { class: "tile-bottom" });
  tile.element = h("div", { class: "tile", role: "group", onclick: () => video.activate(tile) }, tile.video, tile.top, tile.center, tile.bottom);
  tile.preview = new WHEPPreview(tile.video, update => {
    tile.state = update.state;
    tile.error = update.error;
    tile.gesture = update.needsPlaybackGesture;
    if (update.state === "streaming" && !tile.streamingAt) tile.streamingAt = Date.now();
    renderTile(tile, video);
    video.renderBar();
  });
  tile.video.addEventListener("playing", () => {
    tile.gesture = false;
    renderTile(tile, video);
  });
  return tile;
}

function renderTile(tile, video) {
  const stop = handler => event => { event.stopPropagation(); handler(); };
  const failed = tile.state === "failed" || tile.state === "ended";
  tile.element.className = `tile is-${tile.state}${video.isActive(tile) ? " is-active" : ""}`;
  tile.element.setAttribute("aria-label", `${tile.label}：${STATE_LABEL[tile.state] || tile.state}`);
  tile.top.replaceChildren(h("span", { class: "name", title: tile.label }, tile.label), protoTag(tile.kind),
    h("span", { class: "tile-state" }, h("span", { class: `live-dot${tile.state === "streaming" ? " on" : failed ? " err" : ""}` }), STATE_LABEL[tile.state] || tile.state));
  if (failed) {
    tile.center.replaceChildren(icon("alert", "lg"), h("span", {}, errorMessage({ code: tile.error })), h("p", {}, "不会自动重连。可以重试，或关闭该画面。"),
      h("div", { class: "inline" },
        h("button", { type: "button", class: "btn sm", onclick: stop(() => video.retry(tile)) }, icon("refresh", "sm"), "重试"),
        h("button", { type: "button", class: "btn sm", onclick: stop(() => video.close(tile)) }, "关闭")));
  } else if (tile.state === "streaming" && tile.gesture) {
    tile.center.replaceChildren(h("button", { type: "button", class: "btn primary", onclick: stop(() => void tile.video.play().catch(() => {})) }, icon("play", "sm"), "点击播放"));
  } else if (tile.state !== "streaming") {
    tile.center.replaceChildren(h("span", { class: "spinner", "aria-hidden": "true" }), h("span", {}, `${STATE_LABEL[tile.state] || "正在连接"}…`));
  } else {
    tile.center.replaceChildren();
  }
  const fullscreen = document.fullscreenElement === tile.element;
  tile.bottom.replaceChildren(h("span", { class: "meta" }),
    h("button", { type: "button", class: "tile-btn", "aria-label": tile.video.muted ? "打开声音" : "静音", title: tile.video.muted ? "打开声音" : "静音",
      disabled: tile.state !== "streaming", onclick: stop(() => { tile.video.muted = !tile.video.muted; renderTile(tile, video); }) },
    icon(tile.video.muted ? "volume-off" : "volume", "sm")),
    h("a", { class: "tile-btn", href: deviceHash(tile.kind, tile.deviceID), "aria-label": "设备详情", title: "设备详情（离开视频中心，关闭全部画面）", onclick: event => event.stopPropagation() }, icon("info", "sm")),
    h("button", { type: "button", class: "tile-btn", "aria-label": fullscreen ? "退出全屏" : "全屏", title: fullscreen ? "退出全屏（Esc）" : "全屏",
      onclick: stop(() => (document.fullscreenElement ? document.exitFullscreen() : tile.element.requestFullscreen()).catch(() => {})) },
    icon(fullscreen ? "minimize" : "maximize", "sm")),
    h("button", { type: "button", class: "tile-btn", "aria-label": "关闭画面", title: "关闭画面（只释放本画面）", onclick: stop(() => video.close(tile)) }, icon("x", "sm")));
}

function chevron() {
  const glyph = icon("chevron-right", "sm");
  glyph.classList.add("chev");
  return glyph;
}

// 设备树中可播放的目标。GB 通道未点播时，播放会按现有流程先发起点播（用户显式操作）。
function treeItems() {
  const items = [];
  for (const device of store.devices) {
    const channels = device.online ? store.channels.get(device.device_id) || [] : [];
    items.push({
      group: true, kind: "gb", deviceID: device.device_id, name: device.name, online: device.online, reason: device.online ? "" : "设备离线",
      children: channels.map(channel => ({
        kind: "gb", deviceID: device.device_id, key: `gb:${device.device_id}:${channel.channel_id}`, name: channel.name || channel.channel_id,
        label: `${device.name} · ${channel.name || channel.channel_id}`, on: channel.status === "ON",
        note: channel.live?.state === "streaming" ? "点播中" : "",
        reason: channel.status === "ON" ? "" : "通道离线",
        target: { device_id: device.device_id, channel_id: channel.channel_id, live_id: channel.live?.state === "streaming" ? channel.live.live_id : undefined },
      })),
    });
  }
  for (const source of store.sources) {
    const running = source.session?.state === "created";
    items.push({ kind: "rtsp", deviceID: source.source_id, key: `rtsp:${source.source_id}`, name: source.stream_name, label: source.stream_name,
      on: running, reason: running ? "" : "未取流", target: { source_id: source.source_id } });
  }
  for (const device of store.pushDevices) {
    const running = device.state === "verified";
    items.push({ kind: "push", deviceID: device.device_id, key: `push:${device.device_id}`, name: device.name, label: device.name,
      on: running, reason: running ? "" : "未推流", target: { push_device_id: device.device_id } });
  }
  return items;
}

export function mountVideo(root, ctx) {
  let layout = 4;
  let active = 0;
  const slots = [null, null, null, null];
  // 在线 GB 设备默认展开，记录用户收起的设备。
  const collapsed = new Set();
  let query = "";

  const tree = h("div", { class: "tree", role: "tree" });
  const treeCount = h("span", { class: "muted small" });
  const grid = h("div", { class: "tiles l4" });
  const barCount = h("span", { class: "stage-count" });
  const layoutButtons = h("div", { class: "segmented", role: "group", "aria-label": "画面布局" });
  const closeAllButton = h("button", { type: "button", class: "btn sm", onclick: () => closeAll() }, icon("square", "sm"), "全部关闭");

  const video = {
    isActive: tile => slots.indexOf(tile) === active,
    activate(tile) {
      active = slots.indexOf(tile);
      renderGrid();
    },
    close,
    retry(tile) {
      tile.streamingAt = 0;
      void tile.preview.start(tile.target, tile.label).catch(() => {});
    },
    renderBar,
  };

  root.replaceChildren(h("div", { class: "video-page" },
    h("aside", { class: "tree-pane", "aria-label": "设备" },
      h("div", { class: "tree-head" }, h("h2", {}, "设备", treeCount),
        h("label", { class: "search" }, icon("search"), h("input", {
          class: "input", type: "search", placeholder: "搜索设备或通道", "aria-label": "搜索设备或通道",
          oninput: event => { query = event.target.value.trim().toLowerCase(); renderTree(); },
        }))),
      tree,
      h("div", { class: "tree-foot" }, "点击通道或设备开始播放；进入页面不会自动建立播放连接。")),
    h("section", { class: "stage", "aria-label": "播放区域" },
      h("div", { class: "stage-bar" }, h("span", { class: "title" }, h("strong", {}, "视频中心"), barCount), h("span", { class: "spacer" }), layoutButtons, closeAllButton),
      h("div", { class: "grid-wrap" }, grid),
      h("div", { class: "stage-foot" }, icon("info", "sm"),
        h("span", {}, "每个画面是独立的 WHEP 播放器；关闭画面只释放该画面，不停止设备点播或取流。离开视频中心会关闭全部画面，播放失败不会自动重连。")))));

  function tiles() {
    return slots.filter(Boolean);
  }

  function renderBar() {
    const visible = tiles();
    const playing = visible.filter(tile => tile.state === "streaming").length;
    barCount.textContent = visible.length ? `已打开 ${visible.length} 个画面 · 播放中 ${playing}` : "未播放";
    closeAllButton.disabled = visible.length === 0;
    replaceKeepingFocus(layoutButtons, [[1, "grid-1", "单画面"], [4, "grid-4", "四分屏"]].map(([value, glyph, label]) => h("button", {
      type: "button", class: layout === value ? "is-active" : "", "aria-pressed": String(layout === value), dataset: { key: `layout:${value}` },
      onclick: () => switchLayout(value),
    }, icon(glyph, "sm"), label)));
  }

  function renderGrid() {
    grid.className = `tiles l${layout}`;
    const nodes = slots.slice(0, layout).map((tile, index) => {
      if (tile) {
        renderTile(tile, video);
        return tile.element;
      }
      const isActive = index === active;
      return h("div", { class: `tile is-empty${isActive ? " is-active" : ""}`, role: "group", "aria-label": `画面 ${index + 1}：空闲`,
        onclick: () => { active = index; renderGrid(); } },
      h("div", { class: "tile-empty" }, h("span", { class: "slot-mark" }, icon("video", "lg")),
        h("span", {}, isActive ? "从左侧选择通道或设备，在此画面播放" : "空闲画面"),
        layout === 4 ? h("span", { class: "slot" }, `窗口 ${index + 1}${isActive ? " · 当前选中" : ""}`) : null));
    });
    grid.replaceChildren(...nodes);
    // 节点在同一任务内移动不会暂停播放；保险起见恢复正在播放的画面。
    for (const tile of tiles()) {
      if (tile.state === "streaming" && tile.video.paused && !tile.gesture) void tile.video.play().catch(() => {});
    }
    renderBar();
    renderTree();
  }

  function renderTree() {
    const playing = new Set(tiles().map(tile => tile.key));
    const items = treeItems();
    const match = item => !query || item.name.toLowerCase().includes(query) || (item.deviceID || "").toLowerCase().includes(query);
    const leaf = (item, nested) => h("button", {
      type: "button", role: "treeitem", class: `tree-node${nested ? " leaf" : ""}${playing.has(item.key) ? " is-playing" : ""}`,
      disabled: !item.on, title: item.on ? `播放 ${item.label}` : item.reason, dataset: { key: item.key }, onclick: () => pick(item),
    }, h("span", { class: `live-dot${item.on ? " on" : ""}` }), h("span", { class: "name" }, item.name),
    h("span", { class: "inline" }, h("span", { class: "muted small" }, playing.has(item.key) ? "播放中" : item.on ? item.note || "" : item.reason), nested ? null : protoTag(item.kind)));
    const nodes = [];
    for (const item of items) {
      if (!item.group) {
        if (match(item)) nodes.push(leaf(item, false));
        continue;
      }
      const children = item.children.filter(child => match(child) || match(item));
      if (!match(item) && children.length === 0) continue;
      const open = (item.online && !collapsed.has(item.deviceID)) || (query && children.length > 0);
      nodes.push(h("div", { role: "treeitem", "aria-expanded": String(Boolean(open)) },
        h("button", { type: "button", class: "tree-node", "aria-expanded": String(Boolean(open)), dataset: { key: `group:${item.deviceID}` },
          onclick: () => { if (collapsed.has(item.deviceID)) collapsed.delete(item.deviceID); else collapsed.add(item.deviceID); renderTree(); } },
        chevron(), h("span", { class: "name" }, item.name),
        h("span", { class: "inline" }, h("span", { class: "muted small" }, item.online ? String(item.children.length) : item.reason), protoTag("gb"))),
        open ? (item.online ? children.map(child => leaf(child, true)) : h("div", { class: "tree-node leaf muted small" }, h("span"), "设备离线，通道不可用")) : null));
    }
    if (nodes.length === 0) {
      nodes.push(h("p", { class: "muted small tree-empty" }, store.loaded ? (query ? "没有匹配的设备或通道" : "还没有设备") : "正在加载…"));
    }
    replaceKeepingFocus(tree, nodes);
    const online = store.devices.filter(device => device.online).length + store.sources.filter(source => source.session).length + store.pushDevices.filter(device => device.state === "verified").length;
    treeCount.textContent = `${online} / ${store.devices.length + store.sources.length + store.pushDevices.length} 在线`;
  }

  function pick(item) {
    const existing = slots.findIndex(tile => tile?.key === item.key);
    if (existing >= 0 && existing < layout) {
      active = existing;
      renderGrid();
      toast("该画面已在播放", "info");
      return;
    }
    let index = active;
    if (slots[index]) {
      const empty = slots.slice(0, layout).findIndex(tile => !tile);
      if (empty >= 0) index = empty;
    }
    if (slots[index]) {
      toast(`已替换画面 ${index + 1}，原播放已关闭`, "info");
      void slots[index].preview.closeViewer();
    }
    const tile = createTile(item, video);
    slots[index] = tile;
    // 四分屏下连续选择时，焦点移到下一个空闲画面。
    const next = layout === 4 ? [0, 1, 2, 3].find(position => position !== index && !slots[position]) : undefined;
    active = next ?? index;
    renderGrid();
    void tile.preview.start(tile.target, tile.label).catch(() => {});
  }

  function close(tile) {
    const index = slots.indexOf(tile);
    if (index < 0) {
      return;
    }
    slots[index] = null;
    active = index;
    renderGrid();
    void tile.preview.closeViewer().then(error => {
      if (error) toast(errorMessage({ code: "whep_delete_500" }), "danger");
    });
    toast("已关闭该画面，设备点播与其他观看者不受影响", "success");
  }

  function closeAll() {
    const count = tiles().length;
    for (const tile of tiles()) void tile.preview.closeViewer();
    slots.fill(null);
    active = 0;
    renderGrid();
    toast(`已关闭 ${count} 个画面`, "success");
  }

  function switchLayout(next) {
    if (next === layout) {
      return;
    }
    if (next === 1) {
      const keep = slots[active] || tiles()[0] || null;
      const closing = tiles().filter(tile => tile !== keep);
      for (const tile of closing) void tile.preview.closeViewer();
      slots.fill(null);
      slots[0] = keep;
      if (closing.length) toast(`已切换为单画面，关闭了其他 ${closing.length} 个画面`, "info");
    }
    layout = next;
    active = 0;
    renderGrid();
  }

  const onPageHide = () => tiles().forEach(tile => tile.preview.closeForPageHide());
  const onFullscreen = () => tiles().forEach(tile => renderTile(tile, video));
  window.addEventListener("pagehide", onPageHide);
  document.addEventListener("fullscreenchange", onFullscreen);

  renderGrid();
  if (ctx.pendingPlay) {
    const item = ctx.pendingPlay;
    ctx.pendingPlay = null;
    pick(item);
  }

  return {
    update() {
      // 画面播放的媒体流已结束或换代（以开始播放之后发出的快照为准）时结束该画面，不自动重连。
      for (const tile of tiles()) {
        const session = tile.preview.current;
        if (session?.streamID && tile.streamingAt && store.updatedAt > tile.streamingAt && currentStreamID(tile.target) !== session.streamID) {
          void tile.preview.end(session);
        }
      }
      renderTree();
    },
    unmount() {
      window.removeEventListener("pagehide", onPageHide);
      document.removeEventListener("fullscreenchange", onFullscreen);
      const count = tiles().length;
      for (const tile of tiles()) void tile.preview.closeViewer();
      slots.fill(null);
      if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
      if (count) toast(`已离开视频中心，关闭 ${count} 个画面`, "info");
    },
  };
}
