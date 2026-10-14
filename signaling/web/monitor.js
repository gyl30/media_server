// 实时监控：统一设备列表 + 单画面 / 四分屏视频工作区。
// 每个画面对象独占自己的 <video>、WHEPPreview 和 DOM 节点；布局切换只移动画面节点，WHEPPreview 的状态回调
// 绑定画面对象而不是槽位编号。关闭画面只释放该画面的 WHEP 会话，不停止上游点播或拉流。
import { WHEPPreview } from "/whep.js";
import { store, unifiedDevices, deviceHash, channelsOf, syncChannels, loadChannels } from "/store.js";
import { h, icon, protoTag, toast, errorMessage, replaceKeepingFocus } from "/ui.js";
import { playItem } from "/devices.js";

const STATE_LABEL = { preparing: "正在准备", negotiating: "正在建立 WHEP 连接", streaming: "播放中", stopping: "正在关闭", failed: "播放失败", ended: "已结束" };
const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };

// 当前目标对应的运行中媒体流 ID 及该事实的数据时间；用于发现画面所播放的媒体流已结束或换代。
// GB 通道来自按需请求的通道缓存，时间取该缓存的请求时刻，而不是列表快照时间。
function currentStream(target) {
  if (target.source_id) {
    const session = store.sources.find(source => source.source_id === target.source_id)?.session;
    return { id: session?.state === "created" ? session.stream_id : "", at: store.updatedAt };
  }
  if (target.push_device_id) {
    const device = store.pushDevices.find(item => item.device_id === target.push_device_id);
    return { id: device?.state === "verified" ? device.token : "", at: store.updatedAt };
  }
  const cache = store.channels.get(target.device_id);
  const channel = (cache?.channels || []).find(item => item.channel_id === target.channel_id);
  return { id: channel?.live?.state === "streaming" ? channel.live.live_id : "", at: cache?.at ?? 0 };
}

// 设备是否可直接预览及原因。GB 通道在线即可预览：预览时按现有流程发起点播（用户显式操作）。
function previewable(device) {
  if (device.kind === "rtsp") {
    return device.raw.session?.state === "created" ? "" : device.raw.session ? "拉流会话处理中" : "未拉流";
  }
  return device.raw.state === "verified" ? "" : device.raw.state === "issued" ? "等待推流" : "未推流";
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
    const playing = visible.filter(tile => tile.state === "streaming").length;
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
    // 节点在同一任务内移动不会暂停播放；保险起见恢复正在播放的画面。
    for (const tile of tiles()) {
      if (tile.state === "streaming" && tile.video.paused && !tile.gesture) void tile.video.play().catch(() => {});
    }
    renderBar();
    renderList();
  }

  function renderTile(tile, index = slots.indexOf(tile)) {
    const stop = handler => event => { event.stopPropagation(); handler(); };
    const failed = tile.state === "failed" || tile.state === "ended";
    tile.element.className = `tile is-${tile.state}${index === active ? " is-active" : ""}`;
    tile.element.setAttribute("aria-label", `画面 ${index + 1}：${tile.label}，${STATE_LABEL[tile.state] || tile.state}`);
    tile.top.replaceChildren(h("span", { class: `live-dot${tile.state === "streaming" ? " on" : failed ? " err" : ""}` }),
      h("span", { class: "name", title: tile.label }, tile.label), protoTag(tile.protocol),
      h("span", { class: "tile-state" }, STATE_LABEL[tile.state] || tile.state),
      h("button", { type: "button", class: "tile-btn", "aria-label": "关闭画面", title: "关闭画面（只释放本画面）", onclick: stop(() => close(tile)) }, icon("x", "sm")));
    if (failed) {
      tile.center.replaceChildren(icon("alert", "lg"), h("span", {}, errorMessage({ code: tile.error })), h("p", {}, "不会自动重连。可以重试，或关闭该画面。"),
        h("div", { class: "inline" },
          h("button", { type: "button", class: "btn sm", onclick: stop(() => video.retry(tile)) }, icon("refresh", "sm"), "重试"),
          h("button", { type: "button", class: "btn sm", onclick: stop(() => close(tile)) }, "关闭")));
    } else if (tile.state === "streaming" && tile.gesture) {
      tile.center.replaceChildren(h("button", { type: "button", class: "btn primary", onclick: stop(() => void tile.video.play().catch(() => {})) }, icon("play", "sm"), "点击播放"));
    } else if (tile.state !== "streaming") {
      tile.center.replaceChildren(h("span", { class: "spinner", "aria-hidden": "true" }), h("span", {}, `${STATE_LABEL[tile.state] || "正在连接"}…`));
    } else {
      tile.center.replaceChildren();
    }
    const fullscreen = document.fullscreenElement === tile.element;
    tile.bottom.replaceChildren(h("span", { class: "slot-number" }, String(index + 1).padStart(2, "0")), h("span", { class: "meta" }),
      h("button", { type: "button", class: "tile-btn", "aria-label": tile.video.muted ? "打开声音" : "静音", title: tile.video.muted ? "打开声音" : "静音",
        disabled: tile.state !== "streaming", onclick: stop(() => { tile.video.muted = !tile.video.muted; renderTile(tile); }) },
      icon(tile.video.muted ? "volume-off" : "volume", "sm")),
      h("a", { class: "tile-btn", href: deviceHash(tile.kind, tile.deviceID), "aria-label": "设备详情", title: "设备详情（离开实时监控，关闭全部画面）", onclick: event => event.stopPropagation() }, icon("info", "sm")),
      h("button", { type: "button", class: "tile-btn", "aria-label": fullscreen ? "退出全屏" : "全屏", title: fullscreen ? "退出全屏（Esc）" : "全屏",
        onclick: stop(() => (document.fullscreenElement ? document.exitFullscreen() : tile.element.requestFullscreen()).catch(() => {})) },
      icon(fullscreen ? "minimize" : "maximize", "sm")));
  }

  function createTile(item) {
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
      // 已关闭的画面不再属于任何槽位，关闭过程中的状态回调不再渲染。
      if (slots.includes(tile)) {
        renderTile(tile);
        renderBar();
        renderList();
      }
    });
    tile.video.addEventListener("playing", () => {
      tile.gesture = false;
      if (slots.includes(tile)) renderTile(tile);
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
            : !device.channels ? h("p", { class: "monitor-note" }, "正在获取通道…")
            : matchedChannels.length === 0 ? h("p", { class: "monitor-note" }, "暂无通道")
            : matchedChannels.map(channel => sourceRow(playItem(device, channel), channel.name || channel.channel_id,
              channel.status === "ON" ? "" : "通道离线", channel.live?.state === "streaming" ? "点播中" : "可点播", playing))) : null));
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

  // 展开时才请求该 GB 设备的通道列表。
  function toggle(device) {
    if (expanded.has(device.id)) {
      expanded.delete(device.id);
    } else {
      expanded.add(device.id);
      if (device.raw.online) void loadChannels([device.id]);
    }
    renderList();
  }

  // 播放到选中画面：选中画面已有视频时替换（关闭原画面的 viewer）；四分屏中填入空画面后，选中下一个空画面。
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
      // 展开中的在线 GB 设备和正在播放的 GB 设备随轮询同步通道；其余设备不请求。
      syncChannels([...new Set([...[...expanded].filter(id => store.devices.some(device => device.device_id === id && device.online)),
        ...tiles().filter(tile => tile.target.device_id).map(tile => tile.target.device_id)])]);
      // 画面播放的媒体流已结束或换代（以开始播放之后取得的数据为准）时结束该画面，不自动重连。
      for (const tile of tiles()) {
        const session = tile.preview.current;
        const stream = currentStream(tile.target);
        if (session?.streamID && tile.streamingAt && stream.at > tile.streamingAt && stream.id !== session.streamID) {
          void tile.preview.end(session);
        }
      }
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
