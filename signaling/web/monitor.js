import { store, unifiedDevices, syncChannels, loadChannels, sourceReady } from "/store.js";
import { h, icon, toast, errorMessage, replaceKeepingFocus } from "/ui.js";
import { createPlayer, renderPlayer, startPlayer, endIfStreamChanged } from "/player.js";
import { playItem } from "/devices.js";

const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };
const CHANNEL_STATE = { preparing: "正在点播", inviting: "正在点播", stopping: "正在停止" };

function playable(device) {
  return device.kind === "rtsp" ? sourceReady(device.raw) : Boolean(device.raw.media_ready);
}

function channelState(channel) {
  if (channel.status !== "ON") {
    return "通道离线";
  }
  if (channel.live?.media_ready) {
    return "点播中";
  }
  return channel.live ? (channel.live.state === "streaming" ? "等待媒体" : CHANNEL_STATE[channel.live.state] || "") : "";
}

export function mountVideo(root, ctx) {
  let layout = 4;
  let active = 0;
  let query = "";
  const slots = [null, null, null, null];
  const expanded = new Set();

  const list = h("div", { class: "directory", role: "tree", "aria-label": "设备列表" });
  const grid = h("div", { class: "tiles l4" });
  const layoutButtons = h("div", { class: "segmented", role: "group", "aria-label": "分屏" });
  const closeAllButton = h("button", { class: "btn sm", type: "button", onclick: () => closeAll() }, "关闭全部");
  const playerArea = h("section", { class: "card player-area", "aria-label": "监控画面" },
    h("div", { class: "toolbar" }, layoutButtons, h("span", { class: "spacer" }), closeAllButton,
      h("button", { class: "btn sm icon-only", type: "button", title: "全屏", "aria-label": "监控区全屏",
        onclick: () => (document.fullscreenElement ? document.exitFullscreen() : playerArea.requestFullscreen()).catch(() => {}) }, icon("maximize", "sm"))),
    h("div", { class: "stage" }, grid));

  root.replaceChildren(h("div", { class: "page monitor" },
    h("aside", { class: "card resource-pane" },
      h("div", { class: "toolbar" }, h("label", { class: "search" }, icon("search"), h("input", {
        class: "input", type: "search", placeholder: "搜索设备或通道", "aria-label": "搜索设备或通道",
        oninput: event => { query = event.target.value.trim().toLowerCase(); renderList(); },
      }))),
      list),
    playerArea));

  function tiles() {
    return slots.filter(Boolean);
  }

  function renderBar() {
    closeAllButton.disabled = tiles().length === 0;
    replaceKeepingFocus(layoutButtons, [[1, "layout", "单画面"], [4, "grid", "四画面"]].map(([value, glyph, label]) => h("button", {
      type: "button", class: layout === value ? "is-active" : "", "aria-pressed": String(layout === value), dataset: { key: `layout:${value}` },
      onclick: () => switchLayout(value),
    }, icon(glyph, "sm"), label)));
  }

  function selectSlot(index) {
    active = index;
    renderGrid();
  }

  function renderGrid() {
    grid.className = `tiles l${layout}`;
    grid.replaceChildren(...slots.slice(0, layout).map((tile, index) => {
      if (tile) {
        renderTile(tile, index);
        return tile.element;
      }
      return h("div", {
        class: `tile is-empty${index === active ? " is-active" : ""}`, role: "button", tabindex: "0",
        "aria-label": `画面 ${index + 1}，空闲${index === active ? "，当前选中" : ""}`,
        onclick: () => selectSlot(index),
        onkeydown: event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectSlot(index); } },
      }, index === active ? h("span", { class: "tile-hint" }, "从列表选择视频") : null, h("div", { class: "tile-bottom" }, h("span", { class: "slot-number" }, String(index + 1))));
    }));
    for (const tile of tiles()) {
      if (tile.state === "streaming" && tile.video.paused && !tile.gesture) void tile.video.play().catch(() => {});
    }
    renderBar();
    renderList();
  }

  function renderTile(tile, index = slots.indexOf(tile)) {
    renderPlayer(tile, { slot: index + 1, active: index === active, detailLink: true, onClose: () => close(tile) });
  }

  function createTile(item) {
    const tile = createPlayer(item, () => {
      if (slots.includes(tile)) {
        renderTile(tile);
        renderList();
      }
    });
    tile.element.addEventListener("click", () => selectSlot(slots.indexOf(tile)));
    return tile;
  }

  function playButton(item) {
    const slot = slots.findIndex(tile => tile?.key === item.key);
    return h("button", { type: "button", class: `btn sm${slot >= 0 ? " is-current" : ""}`, "aria-pressed": String(slot >= 0), dataset: { key: `play:${item.key}` }, onclick: () => pick(item) },
      icon(slot >= 0 ? "video" : "play", "sm"), slot >= 0 ? `画面 ${slot + 1}` : "播放");
  }

  function directoryRows() {
    const match = (...values) => !query || values.join(" ").toLowerCase().includes(query);
    const rows = [];
    for (const device of unifiedDevices()) {
      if (device.kind === "gb") {
        const channels = device.raw.online ? device.channels || [] : [];
        const selfMatch = match(device.name, device.id, device.type);
        const matchedChannels = channels.filter(channel => selfMatch || match(channel.name, channel.channel_id));
        if (!selfMatch && matchedChannels.length === 0) continue;
        const open = expanded.has(device.id) || Boolean(query && matchedChannels.length && !selfMatch);
        rows.push(h("div", { class: "directory-item", role: "treeitem", "aria-expanded": String(open), dataset: { device: device.id } },
          h("button", { type: "button", class: "directory-row", "aria-expanded": String(open), dataset: { key: `toggle:${device.id}` }, onclick: () => toggle(device) },
            icon(open ? "chevron-down" : "chevron-right", "sm"), icon("camera", "sm"),
            h("span", { class: "directory-text" }, h("strong", { title: device.name }, device.name),
              h("small", {}, !device.raw.online ? "离线" : device.channels ? `${channels.length} 个通道` : "在线"))),
          open && device.raw.online ? h("div", { role: "group" }, !device.channels
            ? h("p", { class: "directory-note" }, device.channelError ? `通道加载失败：${errorMessage(device.channelError)}` : "正在获取通道…")
            : matchedChannels.length === 0 ? h("p", { class: "directory-note" }, "设备未上报通道")
            : matchedChannels.map(channel => {
              const item = playItem(device, channel);
              return h("div", { class: "directory-row child", role: "treeitem" }, icon("video", "sm"),
                h("span", { class: "directory-text" }, h("strong", { title: item.label }, item.label), h("small", {}, channelState(channel))),
                channel.status === "ON" ? playButton(item) : null);
            })) : null));
        continue;
      }
      if (!match(device.name, device.id, device.type)) continue;
      rows.push(h("div", { class: "directory-item", role: "treeitem", dataset: { device: device.id } },
        h("div", { class: "directory-row" }, h("span", { class: "chevron-slot" }), icon(KIND_ICON[device.kind], "sm"),
          h("span", { class: "directory-text" }, h("strong", { title: device.name }, device.name),
            h("small", {}, h("span", {}, device.type), h("span", {}, device.status.label))),
          playable(device) ? playButton(playItem(device)) : null)));
    }
    return rows;
  }

  function renderList() {
    const rows = directoryRows();
    replaceKeepingFocus(list, rows.length ? rows : [emptyList()]);
  }

  function emptyList() {
    return h("p", { class: "directory-note" }, !store.loaded ? "正在加载…" : query ? "未找到设备或通道" : "还没有设备");
  }

  function toggle(device) {
    if (expanded.has(device.id)) {
      expanded.delete(device.id);
    } else {
      expanded.add(device.id);
      if (device.raw.online) void loadChannels([device.id]);
    }
    renderList();
  }

  function pick(item) {
    const existing = slots.findIndex(tile => tile?.key === item.key);
    if (existing >= 0 && existing < layout) {
      selectSlot(existing);
      return;
    }
    const index = active;
    const replaced = slots[index];
    if (replaced) {
      void replaced.preview.closeViewer();
    }
    const tile = createTile(item);
    slots[index] = tile;
    if (!replaced && layout === 4) {
      const next = [1, 2, 3, 0].map(step => (index + step) % 4).find(position => !slots[position]);
      if (next !== undefined) active = next;
    }
    renderGrid();
    startPlayer(tile);
  }

  function close(tile) {
    const index = slots.indexOf(tile);
    if (index < 0) {
      return;
    }
    slots[index] = null;
    selectSlot(index);
    void tile.preview.closeViewer().then(error => {
      if (error) toast(errorMessage({ code: "whep_delete_500" }), "danger");
    });
  }

  function closeAll() {
    const closing = tiles();
    slots.fill(null);
    for (const tile of closing) void tile.preview.closeViewer();
    selectSlot(0);
  }

  function switchLayout(next) {
    if (next === layout) {
      return;
    }
    if (next === 1) {
      const keep = slots[active] || tiles()[0] || null;
      const closing = tiles().filter(tile => tile !== keep);
      slots.fill(null);
      slots[0] = keep;
      for (const tile of closing) void tile.preview.closeViewer();
    }
    layout = next;
    selectSlot(0);
  }

  const onPageHide = () => tiles().forEach(tile => tile.preview.closeForPageHide());
  const onFullscreen = () => tiles().forEach(tile => renderTile(tile));
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
      syncChannels([...new Set([...[...expanded].filter(id => store.devices.some(device => device.device_id === id && device.online)),
        ...tiles().filter(tile => tile.target.device_id).map(tile => tile.target.device_id)])]);
      tiles().forEach(endIfStreamChanged);
      renderList();
    },
    unmount() {
      window.removeEventListener("pagehide", onPageHide);
      document.removeEventListener("fullscreenchange", onFullscreen);
      const closing = tiles();
      slots.fill(null);
      for (const tile of closing) void tile.preview.closeViewer();
      if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
    },
  };
}
