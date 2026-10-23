import { api } from "/api.js";
import { store, unifiedDevices, deviceHash, refresh, loadChannels, pushURLs, sourceReady, pullProtocol, PUSH_PROTOCOLS } from "/store.js";
import {
  h, icon, badge, emptyState, toast, openDialog, confirmAction, errorMessage, replaceKeepingFocus, copyButton,
} from "/ui.js";

const STATUS_FILTERS = [["all", "全部状态"], ["online", "在线"], ["attention", "需要关注"], ["offline", "离线"]];
const filters = { query: "", status: "all" };

export function mountDevices(root, ctx) {
  const search = h("input", {
    class: "input", type: "search", placeholder: "搜索名称、编号或地址", "aria-label": "搜索设备", value: filters.query,
    oninput: event => { filters.query = event.target.value; update(); },
  });
  const status = h("select", { class: "select", "aria-label": "按状态筛选", onchange: event => { filters.status = event.target.value; update(); } },
    STATUS_FILTERS.map(([value, label]) => h("option", { value }, label)));
  status.value = filters.status;
  const total = h("span", { class: "muted" });
  const tbody = h("tbody");
  const body = h("div");

  root.replaceChildren(h("div", { class: "page" }, h("section", { class: "card" },
    h("div", { class: "toolbar" },
      h("label", { class: "search" }, icon("search"), search), status, h("span", { class: "spacer" }), total,
      h("button", { class: "btn primary", type: "button", onclick: () => openDeviceDialog() }, icon("plus"), "添加设备")),
    body)));

  function clearFilters() {
    Object.assign(filters, { query: "", status: "all" });
    search.value = "";
    status.value = "all";
    update();
  }

  function update() {
    const all = unifiedDevices();
    const missing = all.filter(device => device.kind === "gb" && device.raw.online && !store.channels.has(device.id)).map(device => device.id);
    if (missing.length) void loadChannels(missing);
    const query = filters.query.trim().toLowerCase();
    const rows = all.filter(device => (filters.status === "all" || device.status.group === filters.status)
      && (!query || [device.name, device.id, device.type, device.endpoint].join(" ").toLowerCase().includes(query)));
    total.textContent = !store.loaded ? "" : rows.length === all.length ? `共 ${all.length} 个` : `筛选出 ${rows.length} 个，共 ${all.length} 个`;

    if (!store.loaded) {
      body.replaceChildren(store.error
        ? emptyState({ icon: "alert", tone: "danger", title: errorMessage(store.error),
          action: h("button", { class: "btn", type: "button", onclick: () => void refresh() }, icon("refresh"), "重试") })
        : emptyState({ icon: "clock", title: "正在加载…" }));
      return;
    }
    if (rows.length === 0) {
      body.replaceChildren(all.length === 0
        ? emptyState({ icon: "devices", title: "还没有设备" })
        : emptyState({ icon: "search", title: "没有符合条件的设备",
          action: h("button", { class: "btn", type: "button", onclick: clearFilters }, "清除筛选") }));
      return;
    }
    if (!body.contains(tbody)) {
      body.replaceChildren(h("div", { class: "table-scroll" }, h("table", { class: "table" },
        h("thead", {}, h("tr", {}, h("th", {}, "名称"), h("th", {}, "类型"), h("th", {}, "编号或地址"), h("th", {}, "状态"),
          h("th", { class: "c-actions" }, h("span", { class: "sr-only" }, "操作")))),
        tbody)));
    }
    replaceKeepingFocus(tbody, rows.map(device => deviceRow(device, ctx)));
  }

  update();
  return { update, unmount() {} };
}

function deviceRow(device, ctx) {
  const key = `${device.kind}:${device.id}`;
  return h("tr", {},
    h("td", { class: "c-name" }, h("a", { href: deviceHash(device.kind, device.id), dataset: { key: `open:${key}` } }, device.name || "未命名")),
    h("td", { class: "c-type" }, device.type),
    h("td", { class: "c-endpoint" }, h("span", { class: "mono", title: device.endpoint }, device.endpoint),
      device.kind === "gb" && device.channels ? h("span", { class: "muted" }, `${device.channels.length} 个通道`) : null),
    h("td", { class: "c-status" }, badge(device.status.label, device.status.tone),
      device.status.detail ? h("span", { class: "muted" }, device.status.detail) : null),
    h("td", { class: "c-actions" }, rowAction(device, ctx)));
}

function rowAction(device, ctx) {
  const key = `${device.kind}:${device.id}`;
  if (device.kind === "rtsp" && !device.raw.session) {
    return h("button", { class: "btn sm", type: "button", dataset: { key: `start:${key}` }, onclick: () => startSource(device) },
      device.raw.desired_state === "running" ? "重新连接" : "开始拉流");
  }
  if (device.kind === "rtsp" ? sourceReady(device.raw) : device.kind === "push" && device.raw.media_ready) {
    return h("button", { class: "btn sm", type: "button", dataset: { key: `play:${key}` }, onclick: () => ctx.playInCenter(playItem(device)) },
      icon("play", "sm"), "播放");
  }
  return null;
}

export function playItem(device, channel = null) {
  if (device.kind === "gb") {
    return {
      kind: "gb", deviceID: device.id, key: `gb:${device.id}:${channel.channel_id}`, protocol: "GB28181",
      label: channel.name || channel.channel_id, owner: device.name, target: { device_id: device.id, channel_id: channel.channel_id },
    };
  }
  return {
    kind: device.kind, deviceID: device.id, key: `${device.kind}:${device.id}`, protocol: device.protocol, label: device.name,
    target: device.kind === "rtsp" ? { source_id: device.id } : { push_device_id: device.id },
  };
}

export async function startSource(device) {
  try {
    await api.startSource(device.id);
  } catch (error) {
    toast(errorMessage(error), "danger");
  }
  await refresh();
}

export function stopSource(device) {
  confirmAction({
    title: "停止拉流", message: `停止「${device.name}」后，所有观看者都会断开。`, confirmLabel: "停止拉流",
    async action() {
      await api.stopSource(device.id);
      void refresh();
    },
  });
}

export function stopPush(device) {
  confirmAction({
    title: "停止推流", message: `停止「${device.name}」后，所有观看者都会断开，再次推流需要重新生成地址。`, confirmLabel: "停止推流",
    async action() {
      await api.stopPushDevice(device.id);
      void refresh();
    },
  });
}

export async function renewPushURL(device) {
  try {
    await api.renewPushCredential(device.id);
    toast("已生成新地址，5 分钟内有效", "success");
  } catch (error) {
    toast(error.code === "publishing" ? "正在推流，停止后才能重新生成地址" : errorMessage(error), "danger");
  }
  await refresh();
}

export function pushURLBox(url) {
  return h("div", { class: "url-box" }, h("code", {}, url), copyButton(url, "推流地址"));
}

export function deleteDevice(device, ctx) {
  const effects = {
    gb: "所有通道的点播都会停止，观看者会断开。",
    rtsp: "拉流会停止，观看者会断开。",
    push: "推流会停止，接入地址作废，观看者会断开。",
  };
  confirmAction({
    title: "删除设备", message: `删除「${device.name}」后，${effects[device.kind]}`, confirmLabel: "删除",
    async action() {
      if (device.kind === "gb") {
        await api.deleteDevice(device.id);
      } else if (device.kind === "rtsp") {
        await api.deleteSource(device.id);
      } else {
        await api.deletePushDevice(device.id);
      }
      toast(`已删除「${device.name}」`, "success");
      ctx.onDeleted?.(device);
      void refresh();
    },
  });
}

const ADD_OPTIONS = [
  ["gb", "GB28181 设备"], ["rtsp-pull", "RTSP 拉流"], ["rtmp-pull", "RTMP 拉流"], ["whep-pull", "WHEP 拉流"],
  ["rtmp", "RTMP 推流"], ["rtsp", "RTSP 推流"], ["whip", "WHIP 推流"],
];
const PULL_PLACEHOLDERS = {
  "rtsp-pull": "rtsp://192.168.1.100:554/live",
  "rtmp-pull": "rtmp://192.168.1.100:1935/live/stream",
  "whep-pull": "http://192.168.1.100:8080/whep/stream",
};

export function openDeviceDialog(existing = null) {
  const dialog = document.getElementById("device-dialog");
  const raw = existing?.raw;
  let choice = existing ? (existing.kind === "rtsp" ? `${pullProtocol(raw.url)}-pull` : raw.protocol) : "gb";
  let busy = false;
  let prepared = null;
  let prepareError = null;
  let preparing = false;
  const fields = {
    name: h("input", { class: "input", name: "name", autocomplete: "off", maxlength: "64", value: existing?.name || "" }),
    device_id: h("input", { class: "input mono", name: "device_id", inputmode: "numeric", maxlength: "20", autocomplete: "off", placeholder: "20 位数字" }),
    url: h("input", { class: "input mono", name: "url", autocomplete: "off", value: raw?.url || "" }),
    username: h("input", { class: "input", name: "username", autocomplete: "off", value: raw?.username || "" }),
    password: h("input", { class: "input", name: "password", type: "password", autocomplete: "new-password" }),
    clear: h("input", { type: "checkbox", name: "clear_password" }),
  };
  const picker = h("select", { class: "select", name: "protocol", "aria-label": "接入方式" });
  picker.append(...(existing?.kind === "push"
    ? Object.entries(PUSH_PROTOCOLS).map(([value, label]) => h("option", { value }, `${label} 推流`))
    : ADD_OPTIONS.map(([value, label]) => h("option", { value }, label))));
  picker.value = choice;
  picker.addEventListener("change", () => { choice = picker.value; for (const key of Object.keys(errors)) delete errors[key]; render(); });
  const errors = {};
  const failure = h("div", { class: "notice danger", role: "alert", hidden: true }, icon("alert"), h("span"));
  const submit = h("button", { class: "btn primary", type: "submit" }, existing ? "保存" : "添加");
  const cancel = h("button", { class: "btn", type: "button", onclick: () => { if (!busy) dialog.close(); } }, "取消");
  const close = h("button", { class: "icon-btn", type: "button", "aria-label": "关闭", onclick: () => { if (!busy) dialog.close(); } }, icon("x"));
  const content = h("div", { class: "dialog-body" });

  const field = (key, label, input, hint = "", required = false) => h("label", { class: `field${errors[key] ? " invalid" : ""}` },
    h("span", {}, label, required ? h("span", { class: "required", "aria-hidden": "true" }, " *") : null), input,
    errors[key] || hint ? h("small", {}, errors[key] || hint) : null);

  function render() {
    const nodes = [];
    if (existing?.kind === "rtsp") {
      nodes.push(h("div", { class: "field" }, h("span", {}, "接入方式"), h("strong", {}, existing.type)));
    } else {
      nodes.push(field("protocol", "接入方式", picker, "", true));
    }
    nodes.push(field("name", "名称", fields.name, "", true));
    fields.url.placeholder = PULL_PLACEHOLDERS[choice] || "";
    if (choice === "gb") {
      nodes.push(field("device_id", "国标编号", fields.device_id, "", true));
    } else if (choice === "rtmp-pull") {
      nodes.push(field("url", "RTMP 地址", fields.url, "", true));
    } else if (choice === "whep-pull") {
      fields.password.placeholder = raw?.password_set ? "留空保留原值" : "";
      nodes.push(field("url", "WHEP 地址", fields.url, "", true), field("password", "Bearer Token", fields.password));
      if (raw?.password_set) {
        nodes.push(h("label", { class: "check" }, fields.clear, "清除已保存的 Token"));
      }
    } else if (choice === "rtsp-pull") {
      fields.password.placeholder = existing && raw.username ? "留空保留原密码" : "";
      nodes.push(field("url", "RTSP 地址", fields.url, "", true), h("div", { class: "form-grid" },
        field("username", "用户名", fields.username, existing && raw.username ? "清空用户名会同时清除密码" : ""),
        field("password", "密码", fields.password)));
      if (existing && raw.username) {
        nodes.push(h("label", { class: "check" }, fields.clear, "清除已保存的密码"));
      }
    } else if (!existing) {
      if (prepared) {
        nodes.push(h("div", { class: "field" }, h("span", {}, "推流地址"), pushURLBox(pushURLs({ ...prepared, protocol: choice })[0][2]),
          h("small", {}, "添加后 5 分钟内首次接入有效，接入后此地址失效")));
      } else if (prepareError) {
        nodes.push(h("div", { class: "notice danger" }, icon("alert"), h("span", {}, `推流地址生成失败：${errorMessage(prepareError)}`)));
      } else {
        nodes.push(h("p", { class: "muted" }, "正在生成推流地址…"));
        void prepare();
      }
    }
    if (existing?.kind === "rtsp" && raw.desired_state === "running") {
      nodes.push(h("p", { class: "muted" }, "保存后将重新拉流，观看者会断开。"));
    }
    submit.disabled = Boolean(!existing && PUSH_PROTOCOLS[choice] && !prepared);
    nodes.push(failure);
    content.replaceChildren(...nodes);
  }

  async function prepare() {
    if (preparing) {
      return;
    }
    preparing = true;
    try {
      prepared = await api.preparePushCredential();
    } catch (error) {
      prepareError = error;
    }
    if (dialog.open) {
      render();
    }
  }

  const syncPassword = () => {
    const cleared = fields.clear.checked || (existing && raw?.username && !fields.username.value);
    fields.password.disabled = Boolean(cleared);
    if (cleared) fields.password.value = "";
  };
  fields.clear.addEventListener("change", syncPassword);
  fields.username.addEventListener("input", syncPassword);

  async function onSubmit(event) {
    event.preventDefault();
    if (busy) {
      return;
    }
    for (const key of Object.keys(errors)) delete errors[key];
    const name = fields.name.value.trim();
    if (!name) errors.name = "请输入名称";
    if (choice === "gb" && !/^[0-9]{20}$/.test(fields.device_id.value.trim())) errors.device_id = "请输入 20 位数字";
    if (choice === "rtsp-pull") {
      if (!validPullURL(fields.url.value.trim(), "rtsp")) errors.url = "请输入 rtsp:// 地址，账号密码填在下方";
      if (!fields.username.value && fields.password.value) errors.username = "填写密码时需要用户名";
    }
    if (choice === "rtmp-pull" && !validPullURL(fields.url.value.trim(), "rtmp")) {
      errors.url = "请输入 rtmp://主机/应用名/流名，不含账号密码";
    }
    if (choice === "whep-pull" && !validPullURL(fields.url.value.trim(), "http")) {
      errors.url = "请输入 http:// 地址，不含账号密码";
    }
    if (Object.keys(errors).length) {
      render();
      content.querySelector(`[name="${Object.keys(errors)[0]}"]`)?.focus();
      return;
    }
    busy = true;
    for (const button of [submit, cancel, close]) button.disabled = true;
    submit.setAttribute("aria-busy", "true");
    failure.hidden = true;
    try {
      const message = await save(choice, existing, name, fields, prepared);
      dialog.close();
      toast(message, "success");
      void refresh();
    } catch (error) {
      failure.querySelector("span").textContent = errorMessage(error);
      failure.hidden = false;
    } finally {
      busy = false;
      for (const button of [submit, cancel, close]) button.disabled = false;
      submit.setAttribute("aria-busy", "false");
    }
  }

  dialog.replaceChildren(h("form", { novalidate: true, onsubmit: onSubmit },
    h("div", { class: "dialog-head" }, h("h2", {}, existing ? "编辑设备" : "添加设备"), close),
    content,
    h("div", { class: "dialog-foot" }, cancel, submit)));
  dialog.setAttribute("aria-label", existing ? "编辑设备" : "添加设备");
  render();
  const onCancel = event => { if (busy) event.preventDefault(); };
  dialog.addEventListener("cancel", onCancel);
  dialog.addEventListener("close", () => dialog.removeEventListener("cancel", onCancel), { once: true });
  openDialog(dialog, existing ? fields.name : picker);
}

function validPullURL(value, protocol) {
  try {
    const url = new URL(value);
    if (url.protocol !== `${protocol}:` || !url.hostname || url.username || url.password) {
      return false;
    }
    const [app, ...stream] = url.pathname.slice(1).split("/");
    return protocol !== "rtmp" || (Boolean(app) && stream.join("/") !== "");
  } catch {
    return false;
  }
}

async function save(choice, existing, name, fields, prepared) {
  if (choice === "gb") {
    await api.createDevice({ device_id: fields.device_id.value.trim(), name });
    return "已添加，等待设备注册";
  }
  if (existing?.kind === "push" || ["rtmp", "rtsp", "whip"].includes(choice)) {
    if (existing) {
      await api.patchPushDevice(existing.id, choice === existing.raw.protocol ? { name } : { name, protocol: choice });
      return "已保存";
    }
    await api.createPushDevice({ name, protocol: choice, token: prepared.token });
    return "已添加，推流地址 5 分钟内有效";
  }
  const payload = { stream_name: name, url: fields.url.value.trim() };
  if (choice === "rtmp-pull") {
    if (existing) {
      await api.patchSource(existing.id, payload);
      return "已保存";
    }
    await api.createSource(payload);
    return "已添加，正在连接";
  }
  if (choice === "whep-pull") {
    if (fields.clear.checked) {
      payload.password = "";
    } else if (fields.password.value) {
      payload.password = fields.password.value;
    }
    if (existing) {
      await api.patchSource(existing.id, payload);
      return payload.password === undefined ? "已保存" : payload.password ? "已保存，Token 已更新" : "已保存，Token 已清除";
    }
    await api.createSource(payload);
    return "已添加，正在连接";
  }
  const username = fields.username.value;
  const password = fields.password.value;
  if (!existing) {
    if (username) {
      Object.assign(payload, { username, password });
    }
    await api.createSource(payload);
    return "已添加，正在连接";
  }
  payload.username = username;
  if (fields.clear.checked || (!username && existing.raw.username)) {
    payload.password = "";
  } else if (password) {
    payload.password = password;
  }
  await api.patchSource(existing.id, payload);
  if (payload.password === undefined) {
    return "已保存";
  }
  return payload.password ? "已保存，密码已更新" : "已保存，密码已清除";
}
