import { api } from "/api.js";
import { WHEPPreview } from "/whep.js";

const byID = (id) => document.getElementById(id);
const elements = {
  addDevice: byID("add-device-button"),
  deviceDialog: byID("device-dialog"),
  deviceForm: byID("device-form"),
  deviceFormError: byID("device-form-error"),
  deviceID: byID("device-id"),
  deviceName: byID("device-name"),
  saveDevice: byID("save-device-button"),
  deleteDevice: byID("delete-device-button"),
  deviceDetail: byID("device-detail"),
  deviceWelcome: byID("device-welcome"),
  selectedDeviceName: byID("selected-device-name"),
  selectedDeviceStatus: byID("selected-device-status"),
  addSource: byID("add-source-button"),
  channelEmpty: byID("channel-empty"),
  channelRows: byID("channel-rows"),
  clearPassword: byID("clear-password"),
  clearPasswordField: byID("clear-password-field"),
  closeSourceDialog: byID("close-source-dialog"),
  confirmAction: byID("confirm-action-button"),
  confirmDialog: byID("confirm-dialog"),
  confirmMessage: byID("confirm-message"),
  confirmTitle: byID("confirm-title"),
  deviceCount: byID("device-count"),
  deviceEmpty: byID("device-empty"),
  deviceList: byID("device-list"),
  globalStatus: byID("global-status"),
  lastUpdated: byID("last-updated"),
  previewError: byID("preview-error"),
  previewPanel: document.querySelector(".preview-panel"),
  previewPlaceholder: byID("preview-placeholder"),
  previewState: byID("preview-state"),
  previewTarget: byID("preview-target"),
  previewVideo: byID("preview-video"),
  refresh: byID("refresh-button"),
  saveSource: byID("save-source-button"),
  selectedDevice: byID("selected-device"),
  sourceDialog: byID("source-dialog"),
  sourceDialogTitle: byID("source-dialog-title"),
  sourceEmpty: byID("source-empty"),
  sourceForm: byID("source-form"),
  sourceFormError: byID("source-form-error"),
  sourceID: byID("source-id"),
  sourcePassword: byID("source-password"),
  sourceRows: byID("source-rows"),
  sourceStreamName: byID("source-stream-name"),
  sourceURL: byID("source-url"),
  sourceUsername: byID("source-username"),
  stopPreview: byID("stop-preview-button"),
  stopLive: byID("stop-live-button"),
  resumePlayback: byID("resume-playback-button"),
};

const state = {
  channels: [],
  channelsDeviceID: "",
  channelEpoch: 0,
  channelController: null,
  channelSyncUntil: 0,
  devices: [],
  pending: new Set(),
  refreshController: null,
  refreshEpoch: 0,
  selectedDeviceID: "",
  sources: [],
};

const preview = new WHEPPreview(elements.previewVideo, renderPreviewState);
let statusTimer = 0;

function icon(name) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", "icon");
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", `/icons.svg#${name}`);
  svg.append(use);
  return svg;
}

function actionButton(label, action, iconName, variant = "secondary", disabled = false) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `button compact ${variant}`;
  button.dataset.action = action;
  button.disabled = disabled;
  button.append(icon(iconName), document.createTextNode(label));
  return button;
}

function actionIconButton(label, action, iconName, variant = "", disabled = false) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `icon-button table-action ${variant}`.trim();
  button.dataset.action = action;
  button.disabled = disabled;
  button.setAttribute("aria-label", label);
  button.title = label;
  button.append(icon(iconName));
  return button;
}

function textCell(primary, secondary = "", options = {}) {
  const cell = document.createElement("td");
  const first = document.createElement(options.code ? "code" : "span");
  first.className = options.code ? "cell-primary mono" : "cell-primary";
  first.textContent = primary || "-";
  cell.append(first);
  if (secondary) {
    const second = document.createElement(options.secondaryCode ? "code" : "span");
    second.className = options.secondaryCode ? "cell-secondary mono" : "cell-secondary";
    second.textContent = secondary;
    cell.append(second);
  }
  return cell;
}

function badge(label, tone = "neutral") {
  const value = document.createElement("span");
  value.className = `state-badge ${tone}`;
  const dot = document.createElement("span");
  dot.className = "status-dot";
  dot.setAttribute("aria-hidden", "true");
  value.append(dot, document.createTextNode(label));
  return value;
}

function badgeCell(label, tone, detail = "") {
  const cell = document.createElement("td");
  cell.append(badge(label, tone));
  if (detail) {
    const secondary = document.createElement("span");
    secondary.className = "cell-secondary";
    secondary.textContent = detail;
    cell.append(secondary);
  }
  return cell;
}

function toneForState(value) {
  switch (String(value).toLowerCase()) {
    case "online":
    case "on":
    case "streaming":
    case "created":
      return "success";
    case "running":
    case "starting":
    case "preparing":
    case "inviting":
    case "negotiating":
      return "pending";
    case "failed":
    case "error":
    case "unresolved":
      return "danger";
    default:
      return "neutral";
  }
}


function showStatus(message, tone = "info", timeout = 5000) {
  window.clearTimeout(statusTimer);
  elements.globalStatus.textContent = message;
  elements.globalStatus.className = `global-status ${tone}`;
  elements.globalStatus.hidden = false;
  statusTimer = window.setTimeout(() => {
    elements.globalStatus.hidden = true;
  }, timeout);
}

function errorMessage(error) {
  console.debug("管理操作失败", error);
  const messages = {
    device_not_found: "设备不存在", device_exists: "设备已存在", device_offline: "设备当前离线",
    device_stopping: "设备正在清理，请稍后重试", channel_not_found: "通道不存在", channel_offline: "通道当前不可播放",
    play_not_found: "播放链接已失效，请重新播放", live_not_found: "设备取流已结束", live_stopping: "设备取流正在停止，请稍后重试",
    device_delete_failed: "设备暂时无法删除，媒体资源清理未完成，请稍后重试。",
    live_stop_failed: "取流暂时无法停止，请稍后重试", live_start_failed: "设备暂时无法连接，请稍后重试",
    invalid_request: "输入有误，请检查后重试", conflict: "资源正在使用，请稍后重试", not_running: "媒体源尚未开始取流",
    whep_create_404: "播放链接已失效，请重新播放", whep_create_409: "媒体尚未就绪，请稍后重新播放",
    media_ended: "设备已离线或媒体已结束", whep_delete_500: "播放器已关闭，服务器资源尚待清理",
    webrtc_connection_failed: "连接中断，请重新播放", network_error: "无法连接服务器，请稍后重试",
  };
  return messages[error?.code] || (error?.name === "AbortError" ? "操作已取消" : error instanceof TypeError ? "无法连接服务器，请检查网络后重试" : "操作失败，请稍后重试");
}

function sourceIsActive(source) {
  return Boolean(source.session);
}

function restoreFocus(button, fallback = null) {
  if (button && !button.isConnected) {
    const identity = Object.entries(button.dataset);
    button = identity.length ? [...document.querySelectorAll("button")].find(candidate =>
      identity.every(([key, value]) => candidate.dataset[key] === value)) : null;
  }
  if (!button || button.disabled || !button.checkVisibility()) button = fallback;
  if (button && !button.disabled && button.checkVisibility()) button.focus({preventScroll: true});
}

function replaceContent(container, nodes) {
  const active = container.contains(document.activeElement) ? document.activeElement : null;
  const pending = active && (state.pending.has(`source:${active.dataset.sourceId}`)
    || state.pending.has(`channel:${active.dataset.deviceId}:${active.dataset.channelId}`)
    || state.pending.has(`device:${active.dataset.deviceId}`));
  container.replaceChildren(...nodes);
  restoreFocus(active, active && !pending ? (container === elements.sourceRows ? elements.addSource : elements.addDevice) : null);
}

function renderSources() {
  const rows = [];
  for (const source of state.sources) {
    const row = document.createElement("tr");
    const active = sourceIsActive(source);
    const pending = state.pending.has(`source:${source.source_id}`);
    const session = source.session;
    row.append(
      textCell(source.stream_name, source.username ? `用户：${source.username}` : "未设置认证"),
      textCell(source.url, "", { code: true }),
      badgeCell(source.desired_state === "running" ? "正在取流" : "已停止", toneForState(source.desired_state)),
      session
        ? badgeCell(liveLabel(session.state), toneForState(session.state))
        : badgeCell("未取流", "neutral"),
    );
    const actions = document.createElement("td");
    actions.className = "row-actions";
    actions.setAttribute("aria-busy", String(pending));
    const buttons = [];
    if (active) {
      buttons.push(actionButton("停止取流", "stop", "square", "secondary", pending));
      if (session.state === "created") {
        buttons.push(actionButton("播放", "preview", "monitor-play", "secondary", pending));
      }
    } else {
      buttons.push(actionButton("开始取流", "start", "play", "primary", pending));
      if (source.desired_state === "running") {
        buttons.push(actionButton("停止取流", "stop", "square", "secondary", pending));
      }
    }
    buttons.push(actionIconButton(active ? "停止取流后可编辑" : "编辑", "edit", "edit", "", pending || active));
    buttons.push(actionIconButton("删除", "delete", "trash", "danger-icon", pending));
    for (const button of buttons) {
      button.dataset.sourceId = source.source_id;
      actions.append(button);
    }
    row.append(actions);
    rows.push(row);
  }
  replaceContent(elements.sourceRows, rows);
  elements.sourceEmpty.dataset.kind = "empty";
  elements.sourceEmpty.querySelector("strong").textContent = "还没有 RTSP 源";
  elements.sourceEmpty.querySelector("p").textContent = "添加摄像机或媒体源，按需取流与播放";
  elements.sourceEmpty.hidden = state.sources.length !== 0;
  elements.sourceRows.parentElement.hidden = state.sources.length === 0;
}

function renderDevices() {
  const buttons = [];
  for (const device of state.devices) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `device-item${device.device_id === state.selectedDeviceID ? " is-selected" : ""}`;
    button.dataset.deviceId = device.device_id;
    button.setAttribute("aria-pressed", String(device.device_id === state.selectedDeviceID));
    const name = document.createElement("strong");
    name.textContent = device.name;
    const identity = document.createElement("span");
    identity.className = "device-identity mono";
    identity.textContent = device.device_id;
    button.append(name, identity, badge(device.online ? "在线" : "离线", device.online ? "success" : "neutral"));
    buttons.push(button);
  }
  replaceContent(elements.deviceList, buttons);
  elements.deviceEmpty.dataset.kind = "empty";
  elements.deviceEmpty.querySelector("strong").textContent = "还没有设备";
  elements.deviceEmpty.querySelector("p").textContent = "添加设备并完成注册后，即可查看通道和播放画面";
  elements.deviceEmpty.hidden = state.devices.length !== 0;
  elements.deviceCount.textContent = `${state.devices.length} 台设备`;
}

function liveLabel(value) {
  return {preparing: "正在连接", inviting: "正在连接", starting: "正在连接", streaming: "正在取流", created: "正在取流", stopping: "正在停止", cleanup_pending: "正在清理", unresolved: "正在清理"}[value] || "未取流";
}

function renderChannels() {
  const rows = [];
  const device = state.devices.find(item => item.device_id === state.selectedDeviceID);
  const deleting = state.pending.has(`device:${state.selectedDeviceID}`);
  const channels = device?.online && state.channelsDeviceID === state.selectedDeviceID ? state.channels : [];
  elements.deviceDetail.hidden = !device;
  elements.deviceWelcome.hidden = Boolean(device);
  elements.selectedDeviceName.textContent = device?.name || "";
  elements.selectedDevice.textContent = device?.device_id || "";
  elements.selectedDeviceStatus.replaceChildren(badge(device?.online ? "在线" : "离线", device?.online ? "success" : "neutral"));
  elements.deleteDevice.disabled = deleting;
  elements.deleteDevice.setAttribute("aria-busy", String(deleting));
  elements.deleteDevice.lastChild.textContent = deleting ? "正在删除" : "删除设备";
  for (const channel of channels) {
    const live = channel.live;
    const pending = deleting || state.pending.has(`channel:${channel.device_id}:${channel.channel_id}`);
    const row = document.createElement("tr");
    row.dataset.channelId = channel.channel_id;
    row.append(textCell(channel.name || "未命名通道", channel.channel_id, {secondaryCode: true}),
      badgeCell(channel.status === "ON" ? "在线" : "离线", channel.status === "ON" ? "success" : "neutral"),
      badgeCell(liveLabel(live?.state), toneForState(live?.state)));
    const actions = document.createElement("td");
    actions.className = "row-actions";
    actions.setAttribute("aria-busy", String(state.pending.has(`channel:${channel.device_id}:${channel.channel_id}`)));
    if (live) actions.append(actionButton("停止取流", "stop", "square", "secondary", pending));
    actions.append(actionButton("播放", "play", "play", "primary", pending || channel.status !== "ON" || Boolean(live && live.state !== "streaming")));
    for (const button of actions.children) {
      button.dataset.deviceId = channel.device_id;
      button.dataset.channelId = channel.channel_id;
    }
    row.append(actions);
    rows.push(row);
  }
  replaceContent(elements.channelRows, rows);
  elements.channelRows.parentElement.hidden = channels.length === 0;
  elements.channelEmpty.hidden = channels.length !== 0;
  const syncing = device?.online && Date.now() < state.channelSyncUntil;
  elements.channelEmpty.dataset.kind = syncing ? "loading" : "empty";
  elements.channelEmpty.querySelector("strong").textContent = !device?.online ? "设备离线" : syncing ? "正在同步通道…" : "未发现通道";
  elements.channelEmpty.querySelector("p").textContent = !device?.online ? "请检查设备网络与平台接入配置，上线后可查看通道与播放" : syncing ? "设备上线后，通道会自动显示在这里" : "设备尚未上报通道，请检查设备配置";
}

function renderAll() {
  renderSources();
  renderDevices();
  renderChannels();
  renderPlayerButtons();
}

async function refreshChannels(deviceID = state.selectedDeviceID) {
  const epoch = ++state.channelEpoch;
  state.channelController?.abort();
  const device = state.devices.find(item => item.device_id === deviceID);
  if (!device?.online || document.querySelector(".app-layout").dataset.view !== "devices") {
    state.channels = [];
    state.channelsDeviceID = "";
    elements.channelRows.setAttribute("aria-busy", "false");
    renderChannels();
    return;
  }
  const controller = new AbortController();
  state.channelController = controller;
  elements.channelRows.setAttribute("aria-busy", "true");
  try {
    const payload = await api.channels(deviceID, controller.signal);
    if (epoch !== state.channelEpoch || state.selectedDeviceID !== deviceID) return;
    state.channels = payload.channels || [];
    state.channelsDeviceID = deviceID;
    const session = preview.current;
    const target = session?.target;
    if (target?.device_id === deviceID && session.liveID && !state.pending.has(`channel:${deviceID}:${target.channel_id}`)) {
      const channel = state.channels.find(item => item.channel_id === target.channel_id);
      if (channel?.live?.live_id !== session.liveID) void preview.end(session);
    }
    renderChannels();
  } catch (error) {
    if (error.name !== "AbortError" && epoch === state.channelEpoch && state.selectedDeviceID === deviceID) {
      showStatus(errorMessage(error), "danger");
      if (!elements.channelEmpty.hidden) {
        elements.channelEmpty.dataset.kind = "error";
        elements.channelEmpty.querySelector("strong").textContent = "通道暂时无法加载";
        elements.channelEmpty.querySelector("p").textContent = "请检查网络连接后刷新";
      }
    }
  } finally {
    if (epoch === state.channelEpoch) {
      state.channelController = null;
      elements.channelRows.setAttribute("aria-busy", "false");
    }
  }
}

async function refreshSnapshots(options = {}) {
  const epoch = ++state.refreshEpoch;
  if (state.refreshController) {
    state.refreshController.abort();
  }
  const controller = new AbortController();
  state.refreshController = controller;
  elements.refresh.disabled = true;
  elements.refresh.setAttribute("aria-busy", "true");
  elements.deviceList.setAttribute("aria-busy", "true");
  try {
    const [sources, devices] = await Promise.all([
      api.sources(controller.signal),
      api.devices(controller.signal),
    ]);
    if (epoch !== state.refreshEpoch) {
      return;
    }
    const previousDevice = state.devices.find(item => item.device_id === state.selectedDeviceID);
    state.sources = sources.sources || [];
    state.devices = devices.devices || [];
    const session = preview.current;
    if (session?.target.device_id && !state.devices.some(device => device.device_id === session.target.device_id && device.online)) {
      void preview.end(session);
    }
    const currentDevice = state.devices.find(item => item.device_id === state.selectedDeviceID);
    if (currentDevice?.online && !previousDevice?.online) state.channelSyncUntil = Date.now() + 7500;
    const previousDeviceID = state.selectedDeviceID;
    if (!state.devices.some((device) => device.device_id === state.selectedDeviceID)) {
      state.selectedDeviceID = "";
    }
    if (state.selectedDeviceID !== previousDeviceID) {
      state.channelEpoch += 1;
      state.channels = [];
      state.channelsDeviceID = "";
    }
    elements.lastUpdated.dateTime = new Date().toISOString();
    elements.lastUpdated.textContent = `更新于 ${new Date().toLocaleTimeString("zh-CN")}`;
    renderAll();
    await refreshChannels();
    if (!options.quiet) {
      showStatus("已刷新", "success", 2500);
    }
  } catch (error) {
    if (error.name !== "AbortError" && epoch === state.refreshEpoch) {
      showStatus(errorMessage(error), "danger");
      if (!elements.lastUpdated.dateTime) {
        elements.deviceCount.textContent = "暂时无法加载";
        for (const empty of [elements.deviceEmpty, elements.sourceEmpty]) {
          empty.dataset.kind = "error";
          empty.querySelector("strong").textContent = "无法连接服务器";
          empty.querySelector("p").textContent = "请检查网络连接后刷新";
        }
      }
    }
  } finally {
    if (epoch === state.refreshEpoch) {
      state.refreshController = null;
      elements.refresh.disabled = false;
      elements.refresh.setAttribute("aria-busy", "false");
      elements.deviceList.setAttribute("aria-busy", "false");
    }
  }
}

async function runResourceAction(key, action, successMessage) {
  if (state.pending.has(key)) {
    return;
  }
  const trigger = document.activeElement;
  state.pending.add(key);
  renderAll();
  try {
    const result = await action();
    if (result !== null) showStatus(successMessage, "success");
  } catch (error) {
    showStatus(errorMessage(error), "danger");
  } finally {
    state.pending.delete(key);
    await refreshSnapshots({ quiet: true });
    if (document.activeElement === document.body || document.activeElement === trigger) {
      const fallback = key.startsWith("source:")
        ? elements.sourceRows.querySelector(`[data-source-id="${key.slice(7)}"]:not(:disabled)`) || elements.addSource
        : key.startsWith("channel:")
          ? elements.channelRows.querySelector(`button[data-channel-id="${key.split(":")[2]}"]:not(:disabled)`) || elements.addDevice
          : elements.addDevice;
      restoreFocus(trigger, fallback);
    }
  }
}

function sourceByID(sourceID) {
  return state.sources.find((source) => source.source_id === sourceID);
}

function openSourceDialog(source = null, trigger = null) {
  if (elements.saveSource.disabled) return;
  elements.sourceForm.reset();
  elements.sourceFormError.hidden = true;
  elements.sourceID.value = source ? source.source_id : "";
  elements.sourceStreamName.value = source ? source.stream_name : "";
  elements.sourceURL.value = source ? source.url : "";
  elements.sourceUsername.value = source ? source.username : "";
  elements.sourcePassword.value = "";
  elements.clearPassword.checked = false;
  elements.clearPasswordField.hidden = !source;
  elements.sourceDialogTitle.textContent = source ? "编辑源" : "添加源";
  elements.saveSource.textContent = source ? "保存" : "添加源";
  elements.sourceDialog.addEventListener("close", () => restoreFocus(trigger), { once: true });
  elements.sourceDialog.showModal();
  elements.sourceStreamName.focus();
}

function closeSourceDialog() {
  elements.sourceDialog.close();
}

async function submitSourceForm(event) {
  event.preventDefault();
  if (elements.saveSource.disabled) return;
  const editingID = elements.sourceID.value;
  const existing = editingID ? sourceByID(editingID) : null;
  const username = elements.sourceUsername.value;
  const password = elements.sourcePassword.value;
  if (!username && password) {
    elements.sourceFormError.textContent = "设置密码前请输入用户名";
    elements.sourceFormError.hidden = false;
    return;
  }
  const payload = {
    stream_name: elements.sourceStreamName.value.trim(),
    url: elements.sourceURL.value.trim(),
  };
  if (editingID) {
    payload.username = username;
    if (elements.clearPassword.checked || (!username && existing && existing.username)) {
      payload.password = "";
    } else if (password) {
      payload.password = password;
    }
  } else if (username) {
    payload.username = username;
    payload.password = password;
  }

  elements.saveSource.disabled = true;
  elements.closeSourceDialog.disabled = true;
  byID("cancel-source-dialog").disabled = true;
  elements.saveSource.setAttribute("aria-busy", "true");
  elements.saveSource.textContent = editingID ? "正在保存" : "正在添加";
  elements.sourceFormError.hidden = true;
  try {
    if (editingID) {
      await api.patchSource(editingID, payload);
    } else {
      await api.createSource(payload);
    }
    closeSourceDialog();
    showStatus(editingID ? "已更新源" : "已添加源", "success");
    await refreshSnapshots({ quiet: true });
  } catch (error) {
    elements.sourceFormError.textContent = errorMessage(error);
    elements.sourceFormError.hidden = false;
  } finally {
    elements.saveSource.disabled = false;
    elements.closeSourceDialog.disabled = false;
    byID("cancel-source-dialog").disabled = false;
    elements.saveSource.setAttribute("aria-busy", "false");
    elements.saveSource.textContent = editingID ? "保存" : "添加源";
  }
}

function confirmAction(title, message, label) {
  const trigger = document.activeElement;
  elements.confirmTitle.textContent = title;
  elements.confirmMessage.textContent = message;
  elements.confirmAction.textContent = label;
  elements.confirmDialog.returnValue = "";
  elements.confirmDialog.showModal();
  return new Promise((resolve) => {
    elements.confirmDialog.addEventListener("close", () => {
      restoreFocus(trigger);
      resolve(elements.confirmDialog.returnValue === "confirm");
    }, { once: true });
  });
}

function renderPreviewState(update) {
  const labels = {
    failed: "播放失败",
    idle: "未播放",
    ended: "已结束",
    negotiating: "正在连接",
    preparing: "正在连接",
    stopping: "正在关闭",
    streaming: "正在播放",
  };
  const label = labels[update.state] || update.state;
  elements.previewPanel.hidden = update.state === "idle";
  elements.previewPanel.dataset.state = update.state;
  if (update.state === "preparing") {
    elements.previewPanel.scrollIntoView({
      behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth",
      block: "start",
    });
  }
  elements.previewState.replaceChildren(badge(label, toneForState(update.state)));
  elements.previewTarget.textContent = update.target || "";
  elements.stopPreview.disabled = update.state === "stopping";
  elements.resumePlayback.hidden = !update.needsPlaybackGesture;
  renderPlayerButtons();
  elements.previewPlaceholder.hidden = update.state === "streaming";
  elements.previewPlaceholder.querySelector("strong").textContent = label;
  elements.previewError.textContent = update.error ? errorMessage({code: update.error}) : "";
  if (update.error === "whep_create_409" && update.liveID) {
    elements.previewError.textContent += "；若持续失败，请点击通道中的“停止取流”后再播放（会影响该通道的其他观看者）。";
  }
  elements.previewError.hidden = !update.error;
}

function renderPlayerButtons() {
  const session = preview.current;
  const target = session?.target;
  elements.stopLive.hidden = !target?.device_id;
  elements.stopLive.disabled = !session?.liveID || state.pending.has(`channel:${target?.device_id}:${target?.channel_id}`) || state.pending.has(`device:${target?.device_id}`);
}

async function stopLive(deviceID, channelID, liveID) {
  await runResourceAction(`channel:${deviceID}:${channelID}`, async () => {
    await api.stopLive(liveID);
    if (preview.current?.liveID === liveID) await preview.end();
  }, "已停止设备取流");
}

function activateView(view, updateHash = true) {
  const trigger = document.activeElement;
  const valid = ["sources", "devices"].includes(view) ? view : "devices";
  if (document.querySelector(".app-layout").dataset.view !== valid) void preview.closeViewer();
  for (const tab of document.querySelectorAll(".view-tab")) {
    const selected = tab.dataset.view === valid;
    tab.classList.toggle("is-active", selected);
    tab.setAttribute("aria-selected", selected ? "true" : "false");
    tab.tabIndex = selected ? 0 : -1;
  }
  for (const panel of document.querySelectorAll(".view")) {
    const selected = panel.id === `view-${valid}`;
    panel.hidden = !selected;
    panel.classList.toggle("is-active", selected);
  }
  document.querySelector(".app-layout").dataset.view = valid;
  if (trigger !== document.body && !trigger.checkVisibility()) restoreFocus(byID(`tab-${valid}`));
  if (updateHash && window.location.hash !== `#${valid}`) {
    history.replaceState(null, "", `#${valid}`);
  }
}

elements.refresh.addEventListener("click", () => void refreshSnapshots());
elements.addSource.addEventListener("click", (event) => openSourceDialog(null, event.currentTarget));
elements.closeSourceDialog.addEventListener("click", closeSourceDialog);
byID("cancel-source-dialog").addEventListener("click", closeSourceDialog);
elements.sourceForm.addEventListener("submit", submitSourceForm);
elements.sourceDialog.addEventListener("cancel", event => {
  if (elements.saveSource.disabled) event.preventDefault();
});
elements.clearPassword.addEventListener("change", () => {
  elements.sourcePassword.disabled = elements.clearPassword.checked;
  if (elements.clearPassword.checked) {
    elements.sourcePassword.value = "";
  }
});
elements.sourceDialog.addEventListener("close", () => {
  elements.sourcePassword.value = "";
  elements.sourcePassword.disabled = false;
});

const tabList = document.querySelector(".view-tabs");
tabList.addEventListener("click", (event) => {
  const tab = event.target.closest("[data-view]");
  if (tab) {
    activateView(tab.dataset.view);
  }
});

tabList.addEventListener("keydown", (event) => {
  const current = event.target.closest("[role=tab]");
  if (!current) {
    return;
  }
  const tabs = [...tabList.querySelectorAll("[role=tab]")];
  const index = tabs.indexOf(current);
  let next = -1;
  if (event.key === "Home") {
    next = 0;
  } else if (event.key === "End") {
    next = tabs.length - 1;
  } else if (event.key === "ArrowLeft") {
    next = (index - 1 + tabs.length) % tabs.length;
  } else if (event.key === "ArrowRight") {
    next = (index + 1) % tabs.length;
  }
  if (next >= 0) {
    event.preventDefault();
    tabs[next].focus();
    activateView(tabs[next].dataset.view);
  }
});

elements.sourceRows.addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button || button.disabled) {
    return;
  }
  const source = sourceByID(button.dataset.sourceId);
  if (!source) {
    return;
  }
  const key = `source:${source.source_id}`;
  switch (button.dataset.action) {
    case "start":
      await runResourceAction(key, () => api.startSource(source.source_id), "已开始取流");
      break;
    case "stop":
      await runResourceAction(key, async () => {
        await api.stopSource(source.source_id);
        if (preview.current?.target.source_id === source.source_id) await preview.end();
      }, "已停止取流");
      break;
    case "preview":
      await runResourceAction(key, () => preview.start({ source_id: source.source_id }, source.stream_name), "播放器已连接");
      break;
    case "edit":
      openSourceDialog(source, button);
      break;
    case "delete":
      if (await confirmAction("删除 RTSP 源", source.stream_name, "删除源")) {
        await runResourceAction(key, async () => {
          await api.deleteSource(source.source_id);
          if (!preview.current || preview.current.target.source_id === source.source_id) await preview.closeViewer();
        }, "已删除源");
      }
      break;
  }
});

elements.deviceList.addEventListener("click", async event => {
  const button = event.target.closest("button[data-device-id]");
  if (!button || button.dataset.deviceId === state.selectedDeviceID) return;
  const deviceID = button.dataset.deviceId;
  state.selectedDeviceID = deviceID;
  state.channelEpoch += 1;
  state.channelController?.abort();
  state.channels = [];
  state.channelsDeviceID = "";
  state.channelSyncUntil = Date.now() + 7500;
  renderDevices();
  renderChannels();
  try {
    await preview.closeViewer();
    const device = await api.device(deviceID);
    if (state.selectedDeviceID !== deviceID) return;
    const index = state.devices.findIndex(item => item.device_id === deviceID);
    if (index >= 0) state.devices[index] = device;
    renderChannels();
    await refreshChannels(deviceID);
  } catch (error) {
    if (state.selectedDeviceID === deviceID) showStatus(errorMessage(error), "danger");
  }
});

elements.channelRows.addEventListener("click", async (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button || button.disabled) {
    return;
  }
  const deviceID = button.dataset.deviceId;
  const channelID = button.dataset.channelId;
  const channel = state.channels.find((item) => item.device_id === deviceID && item.channel_id === channelID);
  if (!channel) {
    return;
  }
  const key = `channel:${deviceID}:${channelID}`;
  switch (button.dataset.action) {
    case "stop":
      if (channel.live) {
        await stopLive(deviceID, channelID, channel.live.live_id);
      }
      break;
    case "play":
      const device = state.devices.find(item => item.device_id === deviceID);
      await runResourceAction(key, () => preview.start({ device_id: deviceID, channel_id: channelID }, `${device?.name || "设备"} · ${channel.name || channelID}`), "播放器已连接");
      break;
  }
});

elements.stopLive.addEventListener("click", async () => {
  const session = preview.current;
  if (session?.liveID) await stopLive(session.target.device_id, session.target.channel_id, session.liveID);
});

elements.resumePlayback.addEventListener("click", () => {
  void elements.previewVideo.play().catch(error => showStatus(errorMessage(error), "danger"));
});
elements.previewVideo.addEventListener("playing", () => {
  if (preview.current) preview.current.needsPlaybackGesture = false;
  if (document.activeElement === elements.resumePlayback) restoreFocus(elements.stopPreview);
  elements.resumePlayback.hidden = true;
});

elements.stopPreview.addEventListener("click", async () => {
  const target = preview.current?.target;
  const trigger = target?.source_id
    ? elements.sourceRows.querySelector(`[data-source-id="${target.source_id}"][data-action="preview"]`)
    : elements.channelRows.querySelector(`[data-channel-id="${target?.channel_id}"][data-action="play"]`);
  try {
    const cleanupError = await preview.closeViewer();
    if (cleanupError) showStatus(errorMessage({code: "whep_delete_500"}), "danger");
    else showStatus("已关闭播放器", "success");
  } catch (error) {
    showStatus(errorMessage(error), "danger");
  } finally {
    if (document.activeElement === document.body || document.activeElement === elements.stopPreview) {
      restoreFocus(trigger, document.querySelector(".app-layout").dataset.view === "sources" ? elements.addSource : elements.addDevice);
    }
  }
});

window.addEventListener("hashchange", () => activateView(window.location.hash.slice(1), false));
window.addEventListener("pagehide", () => preview.closeForPageHide());

activateView(window.location.hash.slice(1) || "devices", false);
renderPreviewState({ state: "idle", target: "", error: "" });
function openDeviceDialog(event) {
  if (elements.saveDevice.disabled) return;
  const trigger = event.currentTarget;
  elements.deviceForm.reset();
  elements.deviceFormError.hidden = true;
  elements.deviceDialog.addEventListener("close", () => restoreFocus(trigger), { once: true });
  elements.deviceDialog.showModal();
  elements.deviceID.focus();
}

elements.addDevice.addEventListener("click", openDeviceDialog);
for (const id of ["close-device-dialog", "cancel-device-dialog"]) byID(id).addEventListener("click", () => elements.deviceDialog.close());
elements.deviceForm.noValidate = true;
elements.deviceDialog.addEventListener("cancel", event => {
  if (elements.saveDevice.disabled) event.preventDefault();
});
elements.deviceForm.addEventListener("submit", async event => {
  event.preventDefault();
  if (elements.saveDevice.disabled) return;
  const body = {device_id: elements.deviceID.value.trim(), name: elements.deviceName.value.trim()};
  if (!/^[0-9]{20}$/.test(body.device_id) || !body.name) {
    elements.deviceFormError.textContent = "输入有误，请填写 20 位设备编码和设备名称";
    elements.deviceFormError.hidden = false;
    return;
  }
  elements.saveDevice.disabled = true;
  elements.saveDevice.setAttribute("aria-busy", "true");
  elements.saveDevice.textContent = "正在添加";
  for (const id of ["close-device-dialog", "cancel-device-dialog"]) byID(id).disabled = true;
  elements.deviceFormError.hidden = true;
  try {
    await api.createDevice(body);
    elements.deviceDialog.close();
    showStatus("已添加设备", "success");
    await refreshSnapshots({quiet: true});
  } catch (error) {
    elements.deviceFormError.textContent = error.status === 400 ? "输入有误，请检查后重试" : error.status === 409 ? "设备已存在" : "添加失败，请稍后重试";
    elements.deviceFormError.hidden = false;
  } finally {
    elements.saveDevice.disabled = false;
    elements.saveDevice.setAttribute("aria-busy", "false");
    elements.saveDevice.textContent = "添加设备";
    for (const id of ["close-device-dialog", "cancel-device-dialog"]) byID(id).disabled = false;
  }
});

byID("back-devices-button").addEventListener("click", async () => {
  const trigger = elements.deviceList.querySelector(`[data-device-id="${state.selectedDeviceID}"]`);
  state.selectedDeviceID = "";
  state.channelEpoch += 1;
  state.channelController?.abort();
  state.channels = [];
  renderDevices();
  renderChannels();
  restoreFocus(trigger, elements.addDevice);
  await preview.closeViewer().catch(error => console.debug(error));
});

elements.deleteDevice.addEventListener("click", async () => {
  const deviceID = state.selectedDeviceID;
  if (!deviceID || state.pending.has(`device:${deviceID}`)) return;
  if (!await confirmAction("删除设备", "删除设备会停止该设备当前所有播放，是否继续？", "删除设备")) return;
  await runResourceAction(`device:${deviceID}`, async () => {
    await api.deleteDevice(deviceID);
    if (state.selectedDeviceID === deviceID) state.selectedDeviceID = "";
    if (!preview.current || preview.current.target.device_id === deviceID) await preview.closeViewer();
  }, "已删除设备");
});

const polling = window.setInterval(() => {
  if (!state.refreshController) void refreshSnapshots({quiet: true});
}, 2500);
window.addEventListener("pagehide", () => {
  clearInterval(polling);
  state.refreshController?.abort();
  state.channelController?.abort();
});

await refreshSnapshots({quiet: true});
