import { api } from "/api.js";
import { WHEPPreview } from "/whep.js";

const byID = (id) => document.getElementById(id);
const elements = {
  activeRuntimeCount: byID("active-runtime-count"),
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
  eventStatus: byID("event-status"),
  globalStatus: byID("global-status"),
  lastUpdated: byID("last-updated"),
  metricDevices: byID("metric-devices"),
  metricRuntimes: byID("metric-runtimes"),
  metricServers: byID("metric-servers"),
  metricSources: byID("metric-sources"),
  overviewRuntimeEmpty: byID("overview-runtime-empty"),
  overviewRuntimeRows: byID("overview-runtime-rows"),
  previewError: byID("preview-error"),
  previewPanel: document.querySelector(".preview-panel"),
  previewPlaceholder: byID("preview-placeholder"),
  previewState: byID("preview-state"),
  previewStreamID: byID("preview-stream-id"),
  previewTarget: byID("preview-target"),
  previewVideo: byID("preview-video"),
  refresh: byID("refresh-button"),
  runtimeEmpty: byID("runtime-empty"),
  runtimeFilter: byID("runtime-filter"),
  runtimeRows: byID("runtime-rows"),
  saveSource: byID("save-source-button"),
  selectedDevice: byID("selected-device"),
  serverCount: byID("server-count"),
  serverEmpty: byID("server-empty"),
  serverRows: byID("server-rows"),
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
};

const state = {
  channels: [],
  channelsDeviceID: "",
  channelEpoch: 0,
  devices: [],
  eventSource: null,
  pending: new Set(),
  refreshController: null,
  refreshEpoch: 0,
  refreshTimer: 0,
  runtimes: new Map(),
  selectedDeviceID: "",
  servers: [],
  sourceDialogTrigger: null,
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
      return "success";
    case "running":
    case "starting":
    case "preparing":
    case "inviting":
    case "negotiating":
    case "allocating":
      return "pending";
    case "failed":
    case "error":
      return "danger";
    default:
      return "neutral";
  }
}

function formatDate(value) {
  if (!value) {
    return "-";
  }
  const date = new Date(value);
  return Number.isNaN(date.valueOf()) ? "-" : date.toLocaleString();
}

function shortID(value) {
  if (!value || value.length <= 16) {
    return value || "-";
  }
  return `${value.slice(0, 8)}...${value.slice(-4)}`;
}

function showStatus(message, tone = "info", timeout = 5000) {
  window.clearTimeout(statusTimer);
  elements.globalStatus.textContent = message;
  elements.globalStatus.className = `global-status ${tone}`;
  elements.globalStatus.hidden = false;
  if (timeout > 0) {
    statusTimer = window.setTimeout(() => {
      elements.globalStatus.hidden = true;
    }, timeout);
  }
}

function errorCode(error) {
  if (error && typeof error.code === "string") {
    return error.code;
  }
  if (error && error.name === "AbortError") {
    return "request_cancelled";
  }
  return "network_error";
}

function setConnectionState(label, tone) {
  elements.eventStatus.className = `connection-state ${tone}`;
  elements.eventStatus.lastElementChild.textContent = label;
}

function renderMetrics() {
  const activeRuntimes = [...state.runtimes.values()].filter((runtime) => runtime.state !== "stopped");
  const activeSources = state.sources.filter((source) => source.observed && source.observed.state !== "stopped");
  const onlineServers = state.servers.filter((server) => server.online);
  const onlineDevices = state.devices.filter((device) => device.online);
  elements.metricServers.textContent = `${onlineServers.length} / ${state.servers.length}`;
  elements.metricSources.textContent = `${activeSources.length} / ${state.sources.length}`;
  elements.metricDevices.textContent = `${onlineDevices.length} / ${state.devices.length}`;
  elements.metricRuntimes.textContent = String(activeRuntimes.length);
  elements.activeRuntimeCount.textContent = `${activeRuntimes.length} active`;
}

function renderServers() {
  elements.serverRows.replaceChildren();
  for (const server of state.servers) {
    const row = document.createElement("tr");
    row.append(
      textCell(server.server_id, server.instance_id, { secondaryCode: true }),
      textCell(server.media_ip, `HTTP ${server.http_port}`, { code: true }),
      textCell(`RTMP ${server.rtmp_port}`, `RTSP ${server.rtsp_port}`),
      textCell(formatDate(server.last_seen)),
      badgeCell(server.online ? "Online" : "Offline", server.online ? "success" : "neutral"),
    );
    elements.serverRows.append(row);
  }
  elements.serverEmpty.hidden = state.servers.length !== 0;
  elements.serverRows.parentElement.hidden = state.servers.length === 0;
  elements.serverCount.textContent = `${state.servers.length} total`;
}

function renderOverviewRuntimes() {
  const active = [...state.runtimes.values()].filter((runtime) => runtime.state !== "stopped").slice(0, 8);
  elements.overviewRuntimeRows.replaceChildren();
  for (const runtime of active) {
    const row = document.createElement("tr");
    row.append(
      textCell(runtime.stream_name, shortID(runtime.stream_id), { secondaryCode: true }),
      textCell(runtime.protocol.toUpperCase(), runtimeKindLabel(runtime.kind)),
      badgeCell(runtime.state, toneForState(runtime.state), runtime.stage || ""),
      textCell(runtime.server_id, shortID(runtime.instance_id), { secondaryCode: true }),
    );
    elements.overviewRuntimeRows.append(row);
  }
  elements.overviewRuntimeEmpty.hidden = active.length !== 0;
  elements.overviewRuntimeRows.parentElement.hidden = active.length === 0;
}

function sourceIsActive(source) {
  return Boolean(source.observed && source.observed.state !== "stopped");
}

function renderSources() {
  elements.sourceRows.replaceChildren();
  for (const source of state.sources) {
    const row = document.createElement("tr");
    const active = sourceIsActive(source);
    const pending = state.pending.has(`source:${source.source_id}`);
    const observed = source.observed;
    row.append(
      textCell(source.stream_name, source.username ? `user: ${source.username}` : "no credentials"),
      textCell(source.url, "", { code: true }),
      badgeCell(source.desired_state, toneForState(source.desired_state)),
      observed
        ? badgeCell(observed.state, toneForState(observed.state), observed.stage || observed.error || "")
        : badgeCell("Absent", "neutral"),
      observed ? textCell(observed.server_id, shortID(observed.instance_id), { secondaryCode: true }) : textCell("-"),
    );
    const actions = document.createElement("td");
    actions.className = "row-actions";
    const buttons = [];
    if (active) {
      buttons.push(actionButton("Stop", "stop", "square", "secondary", pending));
      if (observed.state === "streaming") {
        buttons.push(actionButton("Preview", "preview", "monitor-play", "secondary", pending));
      }
    } else {
      buttons.push(actionButton("Start", "start", "play", "primary", pending));
      if (source.desired_state === "running") {
        buttons.push(actionButton("Stop", "stop", "square", "secondary", pending));
      }
      buttons.push(actionIconButton("Edit", "edit", "edit", "", pending));
    }
    buttons.push(actionIconButton("Delete", "delete", "trash", "danger-icon", pending));
    for (const button of buttons) {
      button.dataset.sourceId = source.source_id;
      actions.append(button);
    }
    row.append(actions);
    elements.sourceRows.append(row);
  }
  elements.sourceEmpty.hidden = state.sources.length !== 0;
  elements.sourceRows.parentElement.hidden = state.sources.length === 0;
}

function renderDevices() {
  elements.deviceList.replaceChildren();
  for (const device of state.devices) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `device-item${device.device_id === state.selectedDeviceID ? " is-selected" : ""}`;
    button.dataset.deviceId = device.device_id;
    button.setAttribute("aria-pressed", device.device_id === state.selectedDeviceID ? "true" : "false");
    const identity = document.createElement("span");
    identity.className = "device-identity mono";
    identity.textContent = device.device_id;
    const status = badge(device.online ? "Online" : "Offline", device.online ? "success" : "neutral");
    button.append(identity, status);
    elements.deviceList.append(button);
  }
  elements.deviceEmpty.hidden = state.devices.length !== 0;
  elements.deviceCount.textContent = `${state.devices.length} ${state.devices.length === 1 ? "device" : "devices"}`;
}

function renderChannels() {
  elements.channelRows.replaceChildren();
  const device = state.devices.find((item) => item.device_id === state.selectedDeviceID);
  const channels = state.channelsDeviceID === state.selectedDeviceID ? state.channels : [];
  elements.selectedDevice.textContent = device ? device.device_id : "No device selected";
  for (const channel of channels) {
    const live = channel.live;
    const pending = state.pending.has(`channel:${channel.device_id}:${channel.channel_id}`);
    const row = document.createElement("tr");
    row.append(
      textCell(channel.name || "Unnamed channel", channel.channel_id, { secondaryCode: true }),
      badgeCell(channel.status, channel.status === "ON" ? "success" : "neutral", channel.parent_id || ""),
      live ? badgeCell(live.state, toneForState(live.state), shortID(live.stream_id)) : badgeCell("Idle", "neutral"),
    );
    const actions = document.createElement("td");
    actions.className = "row-actions";
    const buttons = [];
    if (live) {
      buttons.push(actionButton("Stop", "stop", "square", "secondary", pending || live.state === "stopping"));
      if (live.state === "streaming") {
        buttons.push(actionButton("Preview", "preview", "monitor-play", "secondary", pending));
      }
    } else {
      buttons.push(actionButton("Start", "start", "play", "primary", pending || !device || !device.online || channel.status !== "ON"));
    }
    for (const button of buttons) {
      button.dataset.deviceId = channel.device_id;
      button.dataset.channelId = channel.channel_id;
      actions.append(button);
    }
    row.append(actions);
    elements.channelRows.append(row);
  }
  const hasDevice = Boolean(device);
  elements.channelEmpty.hidden = hasDevice && channels.length !== 0;
  elements.channelEmpty.textContent = hasDevice ? "No channels reported." : "Select an online device.";
  elements.channelRows.parentElement.hidden = !hasDevice || channels.length === 0;
}

function runtimeDetail(runtime) {
  if (runtime.state === "stopped") {
    return "stopped";
  }
  return "-";
}

function runtimeKindLabel(value) {
  return String(value).replaceAll("_", " ");
}

function renderRuntimes() {
  const filter = elements.runtimeFilter.value;
  const runtimes = [...state.runtimes.values()].filter((runtime) => {
    if (filter === "active") {
      return runtime.state !== "stopped";
    }
    if (filter === "stopped") {
      return runtime.state === "stopped";
    }
    return true;
  });
  elements.runtimeRows.replaceChildren();
  for (const runtime of runtimes) {
    const row = document.createElement("tr");
    row.append(
      textCell(runtime.stream_name, runtime.stream_id, { secondaryCode: true }),
      textCell(runtimeKindLabel(runtime.kind), runtime.source_id ? `source ${shortID(runtime.source_id)}` : ""),
      textCell(runtime.protocol.toUpperCase()),
      badgeCell(runtime.state, toneForState(runtime.state), runtime.stage || ""),
      textCell(runtime.server_id, shortID(runtime.instance_id), { secondaryCode: true }),
      textCell(runtimeDetail(runtime)),
    );
    elements.runtimeRows.append(row);
  }
  elements.runtimeEmpty.hidden = runtimes.length !== 0;
  elements.runtimeRows.parentElement.hidden = runtimes.length === 0;
}

function renderAll() {
  renderMetrics();
  renderServers();
  renderOverviewRuntimes();
  renderSources();
  renderDevices();
  renderChannels();
  renderRuntimes();
}

async function refreshChannels(deviceID = state.selectedDeviceID) {
  const epoch = ++state.channelEpoch;
  if (!deviceID) {
    state.channels = [];
    state.channelsDeviceID = "";
    renderChannels();
    return;
  }
  try {
    const payload = await api.channels(deviceID);
    if (epoch !== state.channelEpoch || state.selectedDeviceID !== deviceID) {
      return;
    }
    state.channels = payload.channels || [];
    state.channelsDeviceID = deviceID;
    renderChannels();
  } catch (error) {
    if (epoch === state.channelEpoch && state.selectedDeviceID === deviceID) {
      state.channels = [];
      state.channelsDeviceID = deviceID;
      renderChannels();
      showStatus(`Channel refresh failed: ${errorCode(error)}`, "danger");
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
  try {
    const [servers, sources, runtimes, devices] = await Promise.all([
      api.mediaServers(controller.signal),
      api.sources(controller.signal),
      api.runtimes(controller.signal),
      api.devices(controller.signal),
    ]);
    if (epoch !== state.refreshEpoch) {
      return;
    }
    state.servers = servers.media_servers || [];
    state.sources = sources.sources || [];
    state.runtimes = new Map((runtimes.runtimes || []).map((runtime) => [runtime.stream_id, runtime]));
    state.devices = devices.devices || [];
    const previousDeviceID = state.selectedDeviceID;
    if (!state.devices.some((device) => device.device_id === state.selectedDeviceID)) {
      state.selectedDeviceID = state.devices.length ? state.devices[0].device_id : "";
    }
    if (state.selectedDeviceID !== previousDeviceID) {
      state.channelEpoch += 1;
      state.channels = [];
      state.channelsDeviceID = "";
    }
    elements.lastUpdated.dateTime = new Date().toISOString();
    elements.lastUpdated.textContent = `Updated ${new Date().toLocaleTimeString()}`;
    renderAll();
    await refreshChannels();
    if (!options.quiet) {
      showStatus("Snapshot refreshed", "success", 2500);
    }
  } catch (error) {
    if (error.name !== "AbortError" && epoch === state.refreshEpoch) {
      showStatus(`Snapshot refresh failed: ${errorCode(error)}`, "danger", 0);
    }
  } finally {
    if (epoch === state.refreshEpoch) {
      state.refreshController = null;
      elements.refresh.disabled = false;
    }
  }
}

function scheduleSnapshotRefresh() {
  if (state.refreshTimer) {
    return;
  }
  state.refreshTimer = window.setTimeout(() => {
    state.refreshTimer = 0;
    void refreshSnapshots({ quiet: true });
  }, 120);
}

async function runResourceAction(key, action, successMessage) {
  if (state.pending.has(key)) {
    return;
  }
  state.pending.add(key);
  renderAll();
  try {
    await action();
    showStatus(successMessage, "success");
  } catch (error) {
    showStatus(`Operation failed: ${errorCode(error)}`, "danger", 0);
  } finally {
    state.pending.delete(key);
    await refreshSnapshots({ quiet: true });
  }
}

function sourceByID(sourceID) {
  return state.sources.find((source) => source.source_id === sourceID);
}

function openSourceDialog(source = null, trigger = null) {
  state.sourceDialogTrigger = trigger;
  elements.sourceForm.reset();
  elements.sourceFormError.hidden = true;
  elements.sourceID.value = source ? source.source_id : "";
  elements.sourceStreamName.value = source ? source.stream_name : "";
  elements.sourceURL.value = source ? source.url : "";
  elements.sourceUsername.value = source ? source.username : "";
  elements.sourcePassword.value = "";
  elements.clearPassword.checked = false;
  elements.clearPasswordField.hidden = !source;
  elements.sourceDialogTitle.textContent = source ? "Edit source" : "Add source";
  elements.saveSource.textContent = source ? "Save changes" : "Create source";
  elements.sourceDialog.showModal();
  elements.sourceStreamName.focus();
}

function closeSourceDialog() {
  elements.sourceDialog.close();
}

async function submitSourceForm(event) {
  event.preventDefault();
  const editingID = elements.sourceID.value;
  const existing = editingID ? sourceByID(editingID) : null;
  const username = elements.sourceUsername.value;
  const password = elements.sourcePassword.value;
  if (!username && password) {
    elements.sourceFormError.textContent = "password_requires_username";
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
  elements.sourceFormError.hidden = true;
  try {
    if (editingID) {
      await api.patchSource(editingID, payload);
    } else {
      await api.createSource(payload);
    }
    closeSourceDialog();
    showStatus(editingID ? "Source updated" : "Source created", "success");
    await refreshSnapshots({ quiet: true });
  } catch (error) {
    elements.sourceFormError.textContent = errorCode(error);
    elements.sourceFormError.hidden = false;
  } finally {
    elements.saveSource.disabled = false;
  }
}

function confirmAction(title, message, label) {
  elements.confirmTitle.textContent = title;
  elements.confirmMessage.textContent = message;
  elements.confirmAction.textContent = label;
  elements.confirmDialog.returnValue = "";
  elements.confirmDialog.showModal();
  return new Promise((resolve) => {
    elements.confirmDialog.addEventListener("close", () => {
      resolve(elements.confirmDialog.returnValue === "confirm");
    }, { once: true });
  });
}

async function startPreview(target, label) {
  try {
    const pending = preview.start(target, label);
    if (window.matchMedia("(max-width: 1180px)").matches) {
      elements.previewPanel.scrollIntoView({
        behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth",
        block: "start",
      });
    }
    const result = await pending;
    if (result) {
      showStatus("Preview started", "success");
    }
  } catch (error) {
    renderPreviewState({ state: "failed", target: label, streamID: "", error: errorCode(error) });
    showStatus(`Preview failed: ${errorCode(error)}`, "danger", 0);
  }
}

function renderPreviewState(update) {
  const labels = {
    allocating: "Allocating",
    failed: "Failed",
    idle: "Idle",
    negotiating: "Negotiating",
    stopping: "Stopping",
    streaming: "Streaming",
  };
  const label = labels[update.state] || update.state;
  elements.previewState.replaceChildren(badge(label, toneForState(update.state)));
  elements.previewTarget.textContent = update.target || "None";
  elements.previewStreamID.textContent = update.streamID || "-";
  elements.stopPreview.disabled = !update.canStop;
  elements.previewPlaceholder.hidden = update.state === "streaming";
  elements.previewError.textContent = update.error || "";
  elements.previewError.hidden = !update.error;
}

function activateView(view, updateHash = true) {
  const valid = ["overview", "sources", "devices", "runtimes"].includes(view) ? view : "overview";
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
  if (updateHash && window.location.hash !== `#${valid}`) {
    history.replaceState(null, "", `#${valid}`);
  }
}

function connectEvents() {
  const events = new EventSource("/api/events");
  events.addEventListener("runtime", (message) => {
    try {
      const runtime = JSON.parse(message.data);
      if (runtime && typeof runtime.stream_id === "string" && runtime.state === "stopped") {
        void preview.stopIfStream(runtime.stream_id).catch((error) => {
          renderPreviewState({ state: "failed", target: "", streamID: runtime.stream_id, error: errorCode(error) });
        });
      }
    } catch {
    }
    scheduleSnapshotRefresh();
  });
  events.addEventListener("open", () => {
    setConnectionState("SSE connected", "is-online");
    void refreshSnapshots({ quiet: true });
  });
  events.addEventListener("error", () => {
    setConnectionState("SSE reconnecting", "is-pending");
  });
  return events;
}

elements.refresh.addEventListener("click", () => void refreshSnapshots());
elements.addSource.addEventListener("click", (event) => openSourceDialog(null, event.currentTarget));
elements.closeSourceDialog.addEventListener("click", closeSourceDialog);
byID("cancel-source-dialog").addEventListener("click", closeSourceDialog);
elements.sourceForm.addEventListener("submit", submitSourceForm);
elements.clearPassword.addEventListener("change", () => {
  elements.sourcePassword.disabled = elements.clearPassword.checked;
  if (elements.clearPassword.checked) {
    elements.sourcePassword.value = "";
  }
});
elements.sourceDialog.addEventListener("close", () => {
  elements.sourcePassword.value = "";
  elements.sourcePassword.disabled = false;
  if (state.sourceDialogTrigger) {
    state.sourceDialogTrigger.focus();
  }
  state.sourceDialogTrigger = null;
});
elements.runtimeFilter.addEventListener("change", renderRuntimes);

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
      await runResourceAction(key, () => api.startSource(source.source_id), "Source start accepted");
      break;
    case "stop":
      await runResourceAction(key, () => api.stopSource(source.source_id), "Source stopped");
      break;
    case "preview":
      await startPreview({ source_id: source.source_id }, source.stream_name);
      break;
    case "edit":
      openSourceDialog(source, button);
      break;
    case "delete":
      if (await confirmAction("Delete RTSP source", source.stream_name, "Delete source")) {
        await runResourceAction(key, () => api.deleteSource(source.source_id), "Source deleted");
      }
      break;
  }
});

elements.deviceList.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-device-id]");
  if (!button || button.dataset.deviceId === state.selectedDeviceID) {
    return;
  }
  state.selectedDeviceID = button.dataset.deviceId;
  state.channelEpoch += 1;
  state.channels = [];
  state.channelsDeviceID = "";
  renderDevices();
  renderChannels();
  void refreshChannels();
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
    case "start":
      await runResourceAction(key, () => api.startChannel(deviceID, channelID), "Channel started");
      break;
    case "stop":
      if (channel.live) {
        await runResourceAction(key, () => api.stopChannel(deviceID, channelID, channel.live.stream_id), "Channel stopped");
      }
      break;
    case "preview":
      await startPreview({ device_id: deviceID, channel_id: channelID }, channel.name || channelID);
      break;
  }
});

elements.stopPreview.addEventListener("click", async () => {
  try {
    await preview.stop();
    showStatus("Preview stopped", "success");
  } catch (error) {
    showStatus(`Preview cleanup failed: ${errorCode(error)}`, "danger", 0);
  }
});

window.addEventListener("hashchange", () => activateView(window.location.hash.slice(1), false));
window.addEventListener("pagehide", () => preview.stopForPageHide());

activateView(window.location.hash.slice(1) || "overview", false);
renderPreviewState({ state: "idle", target: "", streamID: "", error: "" });
await refreshSnapshots({ quiet: true });
state.eventSource = connectEvents();
