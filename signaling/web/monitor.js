import { store, unifiedDevices, syncChannels, loadChannels, sourceReady } from "/store.js";
import { h, icon, protoTag, toast, errorMessage, replaceKeepingFocus } from "/ui.js";
import { createPlayer, renderPlayer, startPlayer, endIfStreamChanged, isPlaying } from "/player.js";
import { playItem } from "/devices.js";

const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };

function previewable(device) {
  if (device.kind === "rtsp") {
    return sourceReady(device.raw) ? "" : device.status.label;
  }
  return device.raw.media_ready ? "" : device.status.label;
}

export function mountVideo(root, ctx) {
  let layout = 4;
  let active = 0;
  let query = "";
  let range = "all";
  const slots = [null, null, null, null];
  const expanded = new Set();
  if (ctx.monitorDevice) {
    expanded.add(ctx.monitorDevice);
  }
  const focusDevice = ctx.monitorDevice;
  ctx.monitorDevice = "";

  const list = h("div", { class: "directory", role: "tree", "aria-label": "设备列表" });
  const listCount = h("span", { class: "muted tiny" });
  const grid = h("div", { class: "tiles l4" });
  const selectedInfo = h("small");
  const footCount = h("span");
  const layoutButtons = h("div", { class: "segmented", role: "group", "aria-label": "选择分屏布局" });
  const stage = h("div", { class: "grid-wrap" }, grid);
  const playerArea = h("section", { class: "card player-area", "aria-label": "监控画面" });
  const closeAllButton = h("button", { class: "btn", type: "button", onclick: () => closeAll() }, icon("x"), "关闭全部画面");

  playerArea.append(
    h("div", { class: "player-toolbar" },
      h("div", { class: "player-toolbar-info" }, h("strong", {}, "监控画面"), selectedInfo),
      h("div", { class: "inline" }, layoutButtons,
        h("button", { class: "btn icon-only", type: "button", title: "监控区全屏", "aria-label": "监控区全屏",
          onclick: () => (document.fullscreenElement ? document.exitFullscreen() : playerArea.requestFullscreen()).catch(() => {}) }, icon("maximize")))),
    h("div", { class: "stage" }, stage),
    h("div", { class: "monitor-foot" }, h("span", { class: "inline" }, icon("info", "sm"), "每个画面是独立的 WHEP 播放器；关闭画面不影响设备点播与其他观看者。"), footCount));

  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "实时监控"), h("p", {}, "统一浏览已接入设备，选择通道或视频源加入选中画面。")),
      h("div", { class: "actions" }, closeAllButton)),
    h("div", { class: "monitor" },
      h("aside", { class: "card resource-pane" },
        h("div", { class: "resource-top" },
          h("div", { class: "resource-title" }, h("span", {}, "设备列表"), listCount),
          h("label", { class: "search" }, icon("search"), h("input", {
            class: "input", type: "search", placeholder: "搜索设备、通道、编号或协议", "aria-label": "搜索设备或通道",
            oninput: event => { query = event.target.value.trim().toLowerCase(); renderList(); },
          })),
          h("div", { class: "filter-line" }, h("span", {}, "显示范围"), h("select", {
            class: "select", "aria-label": "显示范围", onchange: event => { range = event.target.value; renderList(); },
          }, h("option", { value: "all" }, "全部设备"), h("option", { value: "ready" }, "仅在线 / 运行中")))),
        list,
        h("div", { class: "pane-footer" }, icon("info", "sm"), "国标设备展开后查看通道；RTSP、推流设备直接预览。不可播放的项目不提供预览。")),
      playerArea)));

  function tiles() {
    return slots.filter(Boolean);
  }

  function renderBar() {
    const visible = tiles();
    const playing = visible.filter(isPlaying).length;
    const current = slots[active];
    selectedInfo.textContent = `当前选中：画面 ${active + 1} · ${current ? current.label : "空画面"}`;
    footCount.textContent = `已放入 ${visible.length} 路 · 播放中 ${playing}`;
    closeAllButton.disabled = visible.length === 0;
    replaceKeepingFocus(layoutButtons, [[1, "layout", "单画面"], [4, "grid", "四分屏"]].map(([value, glyph, label]) => h("button", {
      type: "button", class: layout === value ? "is-active" : "", "aria-pressed": String(layout === value), dataset: { key: `layout:${value}` },
      onclick: () => switchLayout(value),
    }, icon(glyph, "sm"), label)));
  }

  function renderGrid() {
    grid.className = `tiles l${layout}`;
    grid.replaceChildren(...slots.slice(0, layout).map((tile, index) => {
      if (tile) {
        renderTile(tile, index);
        return tile.element;
      }
      return h("div", {
        class: `tile is-empty${index === active ? " is-active" : ""}`, role: "button", tabindex: "0", "aria-label": `画面 ${index + 1}：空闲`,
        onclick: () => { active = index; renderGrid(); },
        onkeydown: event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); active = index; renderGrid(); } },
      }, h("div", { class: "tile-empty" }, icon("plus", "lg"), h("strong", {}, `画面 ${index + 1}`), h("small", {}, "先选中画面，再从左侧设备列表预览")),
      h("span", { class: "slot-number" }, String(index + 1).padStart(2, "0")));
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
        renderBar();
        renderList();
      }
    });
    tile.element.addEventListener("click", () => {
      active = slots.indexOf(tile);
      renderGrid();
    });
    return tile;
  }

  function directoryRows() {
    const playing = new Set(tiles().map(tile => tile.key));
    const match = (...values) => !query || values.join(" ").toLowerCase().includes(query);
    const rows = [];
    for (const device of unifiedDevices()) {
      const ready = device.status.group === "online";
      if (range === "ready" && !ready) continue;
      if (device.kind === "gb") {
        const channels = device.raw.online ? device.channels || [] : [];
        const selfMatch = match(device.name, device.id, device.protocol, device.mode);
        const matchedChannels = channels.filter(channel => selfMatch || match(channel.name, channel.channel_id));
        if (!selfMatch && matchedChannels.length === 0) continue;
        const open = expanded.has(device.id) || Boolean(query && matchedChannels.length && !selfMatch);
        rows.push(h("div", { class: "monitor-device", role: "treeitem", "aria-expanded": String(open), dataset: { device: device.id } },
          h("button", { type: "button", class: "monitor-device-main", "aria-expanded": String(open), dataset: { key: `toggle:${device.id}` }, onclick: () => toggle(device) },
            h("span", { class: "chev-slot" }, icon(open ? "chevron-down" : "chevron-right", "sm")),
            h("span", { class: "device-symbol" }, icon("camera", "sm")),
            h("span", { class: "device-info" }, h("strong", { title: device.name }, device.name),
              h("small", {}, protoTag("GB28181"), h("span", {}, device.raw.online ? (device.channels ? `${channels.length} 个通道` : "展开查看通道") : "设备离线"))),
            h("span", { class: `device-state${ready ? " ready" : ""}` }, h("span", { class: "dot" }), device.status.label)),
          open ? h("div", { class: "monitor-children" }, !device.raw.online ? h("p", { class: "monitor-note" }, "设备离线，通道不可用")
            : !device.channels ? h("p", { class: "monitor-note" }, device.channelError ? `通道加载失败：${errorMessage(device.channelError)}` : "正在获取通道…")
            : matchedChannels.length === 0 ? h("p", { class: "monitor-note" }, "暂无通道")
            : matchedChannels.map(channel => sourceRow(playItem(device, channel), channel.name || channel.channel_id,
              channel.status === "ON" ? "" : "通道离线", channel.live?.media_ready ? "点播中" : "可点播", playing))) : null));
        continue;
      }
      if (!match(device.name, device.id, device.protocol, device.mode)) continue;
      const reason = previewable(device);
      rows.push(h("div", { class: "monitor-device", role: "treeitem", dataset: { device: device.id } },
        h("div", { class: "monitor-device-main" },
          h("span", { class: "chev-slot" }),
          h("span", { class: "device-symbol" }, icon(KIND_ICON[device.kind], "sm")),
          h("span", { class: "device-info" }, h("strong", { title: device.name }, device.name),
            h("small", {}, protoTag(device.protocol), h("span", {}, device.status.label))),
          previewButton(playItem(device), reason, playing))));
    }
    return rows;
  }

  function sourceRow(item, name, reason, note, playing) {
    return h("div", { class: `monitor-source${reason ? " unavailable" : ""}` },
      h("span", { class: "source-icon" }, icon("video", "sm")),
      h("span", { class: "source-info" }, h("strong", { title: name }, name), h("small", {}, reason || (playing.has(item.key) ? "播放中" : note))),
      previewButton(item, reason, playing));
  }

  function previewButton(item, reason, playing) {
    if (reason) {
      return h("span", { class: "monitor-off" }, reason);
    }
    return h("button", { type: "button", class: "monitor-play", title: "播放到选中画面", dataset: { key: `play:${item.key}` }, onclick: () => pick(item) },
      icon("play", "sm"), playing.has(item.key) ? "播放中" : "预览");
  }

  function renderList() {
    const rows = directoryRows();
    replaceKeepingFocus(list, rows.length ? rows : [h("div", { class: "monitor-empty" }, icon("search"),
      h("strong", {}, store.loaded ? "未找到设备或通道" : "正在加载…"), store.loaded ? h("span", {}, "尝试修改搜索条件或显示范围") : null)]);
    listCount.textContent = store.loaded ? `${rows.length} 个设备 / 源` : "";
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
      active = existing;
      renderGrid();
      toast("该视频已在画面中播放", "info");
      return;
    }
    const index = active;
    const replaced = slots[index];
    if (replaced) {
      void replaced.preview.closeViewer();
      toast(`已替换画面 ${index + 1}，原播放已关闭`, "info");
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
    active = index;
    renderGrid();
    void tile.preview.closeViewer().then(error => {
      if (error) toast(errorMessage({ code: "whep_delete_500" }), "danger");
    });
    toast(`已关闭画面 ${index + 1}，设备点播与其他观看者不受影响`, "success");
  }

  function closeAll() {
    const count = tiles().length;
    const closing = tiles();
    slots.fill(null);
    active = 0;
    for (const tile of closing) void tile.preview.closeViewer();
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
      slots.fill(null);
      slots[0] = keep;
      for (const tile of closing) void tile.preview.closeViewer();
      if (closing.length) toast(`已切换为单画面，关闭了其他 ${closing.length} 个画面`, "info");
    }
    layout = next;
    active = 0;
    renderGrid();
  }

  const onPageHide = () => tiles().forEach(tile => tile.preview.closeForPageHide());
  const onFullscreen = () => tiles().forEach(tile => renderTile(tile));
  window.addEventListener("pagehide", onPageHide);
  document.addEventListener("fullscreenchange", onFullscreen);

  renderGrid();
  if (focusDevice) {
    void loadChannels([focusDevice]);
    list.querySelector(`[data-device="${CSS.escape(focusDevice)}"]`)?.scrollIntoView({ block: "nearest" });
  }
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
      if (closing.length) toast(`已离开实时监控，关闭 ${closing.length} 个画面`, "info");
    },
  };
}
