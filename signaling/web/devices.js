// 设备管理：GB28181 设备、RTSP 拉流源、推流设备（RTMP / RTSP / WHIP）统一列表，以及添加、编辑、删除。
import { api } from "/api.js";
import { store, unifiedDevices, deviceHash, refresh, loadChannels, pushURLs, PUSH_PROTOCOLS } from "/store.js";
import {
  h, icon, badge, protoTag, emptyState, toast, openDialog, confirmAction, errorMessage, replaceKeepingFocus, copyButton,
} from "/ui.js";

const PROTOCOL_FILTERS = [["all", "全部协议"], ["gb", "GB28181"], ["rtsp-pull", "RTSP 拉流"], ["rtmp-push", "RTMP 推流"],
  ["rtsp-push", "RTSP 推流"], ["whip-push", "WHIP 推流"], ["push-unspecified", "推流（未指定）"]];
const filters = { query: "", protocol: "all", status: "all" };
const KIND_ICON = { gb: "camera", rtsp: "link", push: "radio" };

export function mountDevices(root, ctx) {
  const search = h("input", {
    class: "input", type: "search", placeholder: "搜索名称、编号、协议或地址", "aria-label": "搜索设备", value: filters.query,
    oninput: event => { filters.query = event.target.value; update(); },
  });
  const protocol = h("select", { class: "select", "aria-label": "按协议筛选", onchange: event => { filters.protocol = event.target.value; update(); } },
    PROTOCOL_FILTERS.map(([value, label]) => h("option", { value }, label)));
  protocol.value = filters.protocol;
  const status = h("select", { class: "select", "aria-label": "按状态筛选", onchange: event => { filters.status = event.target.value; update(); } },
    h("option", { value: "all" }, "全部状态"), h("option", { value: "online" }, "在线 / 运行中"),
    h("option", { value: "offline" }, "离线 / 未运行"), h("option", { value: "attention" }, "需关注"));
  status.value = filters.status;
  const total = h("span", { class: "muted tiny" });
  const tbody = h("tbody");
  const body = h("div");
  const foot = h("div");

  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "设备管理"), h("p", {}, "所有接入设备和视频源在同一列表中管理；不同协议使用各自的参数与操作。")),
      h("div", { class: "actions" }, h("button", { class: "btn primary", type: "button", onclick: () => openDeviceDialog() }, icon("plus"), "添加设备"))),
    h("div", { class: "card" },
      h("div", { class: "toolbar" },
        h("div", { class: "toolbar-left" }, h("label", { class: "search" }, icon("search"), search), protocol, status),
        h("div", { class: "toolbar-right" }, total)),
      body, foot)));

  function update() {
    const all = unifiedDevices();
    // 只为还没有通道数据的在线 GB 设备请求一次通道列表，不随轮询全量刷新。
    const missing = all.filter(device => device.kind === "gb" && device.raw.online && !store.channels.has(device.id)).map(device => device.id);
    if (missing.length) void loadChannels(missing);
    const query = filters.query.trim().toLowerCase();
    const rows = all.filter(device => (filters.protocol === "all" || device.filter === filters.protocol)
      && (filters.status === "all" || device.status.group === filters.status)
      && (!query || [device.name, device.id, device.protocol, device.mode, device.endpoint].join(" ").toLowerCase().includes(query)));
    total.textContent = store.loaded ? `共 ${all.length} 个接入对象` : "";

    if (!store.loaded) {
      body.replaceChildren(store.error
        ? emptyState({ icon: "wifi-off", tone: "danger", title: "设备列表暂时无法加载", text: errorMessage(store.error),
          action: h("button", { class: "btn", type: "button", onclick: () => void refresh() }, icon("refresh"), "重试") })
        : skeletonTable());
      foot.replaceChildren();
      return;
    }
    if (rows.length === 0) {
      body.replaceChildren(all.length === 0
        ? emptyState({ icon: "camera", title: "还没有设备", text: "添加 GB28181 设备、RTSP 拉流源或 RTMP / RTSP / WHIP 推流设备。",
          action: h("button", { class: "btn primary", type: "button", onclick: () => openDeviceDialog() }, icon("plus"), "添加设备") })
        : emptyState({ icon: "search", title: "没有符合条件的设备", text: "尝试调整搜索关键词、协议或状态筛选。",
          action: h("button", { class: "btn", type: "button", onclick: () => { Object.assign(filters, { query: "", protocol: "all", status: "all" }); search.value = ""; protocol.value = "all"; status.value = "all"; update(); } }, "清除筛选") }));
      foot.replaceChildren();
      return;
    }
    if (!body.contains(tbody)) {
      body.replaceChildren(h("div", { class: "table-scroll" }, h("table", { class: "table cards" },
        h("thead", {}, h("tr", {}, ["设备名称", "协议", "接入方式", "地址 / 设备编号", "状态", "通道", "操作"].map((label, index) => h("th", { class: index === 2 ? "c-mode" : index === 5 ? "num hide-sm" : "" }, label)))),
        tbody)));
    }
    replaceKeepingFocus(tbody, rows.map(device => deviceRow(device, ctx)));
    const filtered = filters.protocol !== "all" || filters.status !== "all" || query;
    foot.replaceChildren(h("div", { class: "table-foot" },
      h("span", {}, filtered ? `筛选出 ${rows.length} 条，共 ${all.length} 条` : `${all.length} 条记录`),
      store.error ? h("span", { class: "stale" }, icon("alert", "sm"), "最近一次刷新失败，显示的是上次结果") : h("span", {}, "按名称排列")));
  }

  update();
  return { update, unmount() {} };
}

function skeletonTable() {
  return h("div", { class: "table-scroll", "aria-busy": "true" }, h("table", { class: "table" }, h("tbody", {},
    [0, 1, 2, 3, 4].map(index => h("tr", {},
      h("td", {}, h("div", { class: "skeleton", style: `width:${60 - index * 5}%` })),
      h("td", {}, h("div", { class: "skeleton", style: "width:64px" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:70%" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:56px" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:96px" })))))));
}

function deviceRow(device, ctx) {
  const key = `${device.kind}:${device.id}`;
  return h("tr", {},
    h("td", { class: "c-name" }, h("div", { class: "device-name" },
      h("span", { class: "device-square" }, icon(KIND_ICON[device.kind])),
      h("div", { class: "cell-title" },
        h("a", { class: "link-btn", href: deviceHash(device.kind, device.id), dataset: { key: `open:${key}` } }, device.name || "未命名"),
        device.kind === "gb" ? null : h("span", { class: "table-sub mono" }, device.id)))),
    h("td", { class: "c-proto" }, protoTag(device.protocol)),
    h("td", { class: "c-mode" }, device.mode),
    h("td", { class: "c-endpoint" }, h("div", { class: "endpoint", title: device.endpoint }, device.endpoint)),
    h("td", { class: "c-status" }, h("div", { class: "cell-title" },
      h("span", {}, badge(device.status.label, device.status.tone)),
      device.status.detail ? h("span", { class: "table-sub" }, device.status.detail) : null)),
    h("td", { class: "num hide-sm" }, device.kind === "gb" ? (device.channels ? String(device.channels.length) : h("span", { class: "muted", title: device.channelError ? errorMessage(device.channelError) : "" }, device.channelError ? "获取失败" : device.raw.online ? "…" : "—")) : h("span", { class: "muted" }, "—")),
    h("td", { class: "c-actions" }, h("div", { class: "text-actions" }, rowActions(device, ctx))));
}

function rowActions(device, ctx) {
  const key = `${device.kind}:${device.id}`;
  const action = (label, name, onclick, options = {}) => h("button", {
    type: "button", class: options.neg ? "neg" : "", disabled: options.disabled, title: options.title, dataset: { key: `${name}:${key}`, action: name }, onclick,
  }, label);
  const links = [h("a", { href: deviceHash(device.kind, device.id), dataset: { key: `detail:${key}` } }, "详情")];
  if (device.kind === "gb") {
    links.push(action("通道", "channels", () => ctx.openMonitorDevice(device.id)));
  } else if (device.kind === "rtsp") {
    const source = device.raw;
    const editable = editState(device);
    links.push(action("编辑", "edit", () => openDeviceDialog(device), { disabled: !editable.ok, title: editable.reason }));
    links.push(source.session || source.desired_state === "running"
      ? action("停止拉流", "stop", () => stopSource(device), { neg: true })
      : action("开始拉流", "start", () => startSource(device)));
    if (source.session?.state === "created") links.push(action("预览", "preview", () => ctx.playInCenter(playItem(device))));
  } else {
    links.push(action("编辑", "edit", () => openDeviceDialog(device)));
    if (device.raw.state === "publishing") links.push(action("预览", "preview", () => ctx.playInCenter(playItem(device))));
  }
  links.push(action("删除", "delete", () => deleteDevice(device, ctx), { neg: true }));
  return links;
}

// 带到实时监控的播放项；target 沿用 whep.js 的结构。
export function playItem(device, channel = null) {
  if (device.kind === "gb") {
    return {
      kind: "gb", deviceID: device.id, key: `gb:${device.id}:${channel.channel_id}`, protocol: "GB28181", label: `${device.name} · ${channel.name || channel.channel_id}`,
      target: { device_id: device.id, channel_id: channel.channel_id, live_id: channel.live?.state === "streaming" ? channel.live.live_id : undefined },
    };
  }
  return {
    kind: device.kind, deviceID: device.id, key: `${device.kind}:${device.id}`, protocol: device.protocol, label: device.name,
    target: device.kind === "rtsp" ? { source_id: device.id } : { push_device_id: device.id },
  };
}

// GB28181 没有修改接口；RTSP 源取流期间不可修改（与服务端约束一致）。
export function editState(device) {
  if (device.kind === "gb") {
    return { ok: false, reason: "GB28181 设备暂不支持编辑" };
  }
  if (device.kind === "rtsp" && device.raw.session) {
    return { ok: false, reason: "停止拉流后可编辑" };
  }
  return { ok: true, reason: "" };
}

export async function startSource(device) {
  try {
    await api.startSource(device.id);
    toast("已请求开始拉流", "success");
  } catch (error) {
    toast(errorMessage(error), "danger");
  }
  await refresh();
}

export function stopSource(device) {
  confirmAction({
    title: "停止拉流", message: `停止「${device.name}」的拉流后，正在观看的用户都会断开，已签发的播放地址随之失效。`, confirmLabel: "停止拉流",
    async action() {
      await api.stopSource(device.id);
      toast("已停止拉流", "success");
      void refresh();
    },
  });
}

export function stopPush(device) {
  confirmAction({
    title: "停止推流", message: `停止「${device.name}」的推流后，正在观看的用户都会断开；接入地址已使用，再次推流需要重新生成地址。`, confirmLabel: "停止推流",
    async action() {
      await api.stopPushDevice(device.id);
      toast("已停止推流", "success");
      void refresh();
    },
  });
}

// 重新生成接入地址：旧地址立即失效；推流中由服务端拒绝。
export async function renewPushURL(device) {
  try {
    await api.renewPushCredential(device.id);
    toast("已生成新的接入地址，5 分钟内有效", "success");
  } catch (error) {
    toast(error.code === "publishing" ? "正在推流，停止推流后才能重新生成地址" : errorMessage(error), "danger");
  }
  await refresh();
}

// 接入地址行：按设备协议只列一种；未指定协议时三种共用同一凭据。
export function pushURLRows(urls) {
  return h("div", { class: "url-rows bordered" }, urls.map(([, label, url]) => h("div", { class: "url-row" },
    h("div", { class: "url-proto" }, h("strong", {}, label)), h("div", { class: "url-side" }, copyButton(url, ` ${label} 推流地址`)), h("div", { class: "url-value" }, url))));
}

export function deleteDevice(device, ctx) {
  const messages = {
    gb: `删除「${device.name}」会停止该设备所有通道的点播，正在观看的用户都会断开。`,
    rtsp: `删除「${device.name}」会先停止拉流，正在观看的用户会断开。`,
    push: `删除「${device.name}」会停止当前推流并作废接入地址，正在观看的用户会断开。`,
  };
  confirmAction({
    title: "删除设备", message: messages[device.kind], confirmLabel: "删除设备",
    async action() {
      if (device.kind === "gb") {
        await api.deleteDevice(device.id);
      } else if (device.kind === "rtsp") {
        await api.deleteSource(device.id);
      } else {
        await api.deletePushDevice(device.id);
      }
      toast("已删除设备", "success");
      ctx.onDeleted?.(device);
      void refresh();
    },
  });
}

const ADD_OPTIONS = [
  ["gb", "GB28181 · SIP 注册"], ["rtsp-pull", "RTSP · 主动拉流"],
  ["rtmp", "RTMP · 主动推流"], ["rtsp", "RTSP · 主动推流"], ["whip", "WHIP · 主动推流"],
];
const PUSH_HELP = {
  rtmp: "编码器、OBS 等推流客户端使用下面的 RTMP 地址推流。",
  rtsp: "推流客户端使用下面的 RTSP 地址，通过 ANNOUNCE / RECORD 发布。",
  whip: "兼容 WHIP 的客户端向下面的 HTTP 地址发起 SDP 协商。",
};

// existing 为空时是添加；否则编辑（RTSP 拉流源或推流设备）。
export function openDeviceDialog(existing = null) {
  const dialog = document.getElementById("device-dialog");
  const raw = existing?.raw;
  // choice：gb / rtsp-pull / rtmp / rtsp / whip；推流设备编辑时为当前协议（未指定为空串）。
  let choice = existing ? (existing.kind === "rtsp" ? "rtsp-pull" : raw.protocol) : "gb";
  let busy = false;
  // 添加推流设备时预生成的接入凭据；三种协议的地址共用它，切换协议不重新生成。
  let prepared = null;
  let prepareError = null;
  let preparing = false;
  const fields = {
    name: h("input", { class: "input", name: "name", autocomplete: "off", maxlength: "64", value: existing?.name || "", placeholder: "请输入设备或视频源名称" }),
    device_id: h("input", { class: "input mono", name: "device_id", inputmode: "numeric", maxlength: "20", autocomplete: "off", placeholder: "20 位数字，例如 34020000001320000001" }),
    url: h("input", { class: "input mono", name: "url", autocomplete: "off", placeholder: "rtsp://192.168.1.100:554/live", value: raw?.url || "" }),
    username: h("input", { class: "input", name: "username", autocomplete: "off", placeholder: "可选", value: raw?.username || "" }),
    password: h("input", { class: "input", name: "password", type: "password", autocomplete: "new-password", placeholder: existing ? "留空则保留原密码" : "可选" }),
    clear: h("input", { type: "checkbox", name: "clear_password" }),
  };
  const picker = h("select", { class: "select", name: "protocol", "aria-label": "接入协议" });
  if (existing?.kind === "push") {
    picker.append(...(raw.protocol ? [] : [h("option", { value: "" }, "未指定")]),
      ...Object.entries(PUSH_PROTOCOLS).map(([value, label]) => h("option", { value }, `${label} · 主动推流`)));
  } else {
    picker.append(...ADD_OPTIONS.map(([value, label]) => h("option", { value }, label)));
  }
  picker.value = choice;
  picker.addEventListener("change", () => { choice = picker.value; for (const key of Object.keys(errors)) delete errors[key]; render(); });
  const errors = {};
  const failure = h("div", { class: "notice warn", role: "alert", hidden: true }, icon("alert"), h("span"));
  const submit = h("button", { class: "btn primary", type: "submit" }, existing ? "保存" : "确定添加");
  const cancel = h("button", { class: "btn", type: "button", onclick: () => { if (!busy) dialog.close(); } }, "取消");
  const close = h("button", { class: "icon-btn", type: "button", "aria-label": "关闭", onclick: () => { if (!busy) dialog.close(); } }, icon("x"));
  const content = h("div", { class: "dialog-body" });

  const field = (key, label, input, hint = "", required = false) => h("label", { class: `field${errors[key] ? " invalid" : ""}` },
    h("span", {}, label, required ? h("span", { class: "danger-text" }, " *") : null), input, errors[key] || hint ? h("small", {}, errors[key] || hint) : null);

  function render() {
    const nodes = [];
    if (!existing) {
      nodes.push(h("p", {}, "先选择设备接入协议，下面只显示该协议实际需要的配置项。"));
    }
    if (existing?.kind === "rtsp") {
      nodes.push(h("div", { class: "field" }, h("span", {}, "接入协议"), h("div", { class: "inline" }, protoTag("RTSP"), h("span", { class: "muted tiny" }, "主动拉流 · 接入方式创建后不可修改"))));
    } else {
      nodes.push(field("protocol", "接入协议", picker, existing ? "协议用于展示和选择接入地址，不会限制服务端可接受的推流协议" : "", true));
    }
    nodes.push(field("name", "名称", fields.name, "", true));
    if (choice === "gb") {
      nodes.push(field("device_id", "国标设备编号", fields.device_id, "设备需要使用该编号主动发起 SIP 注册；创建成功不等于已上线。", true));
    } else if (choice === "rtsp-pull") {
      nodes.push(field("url", "RTSP 地址", fields.url, "请输入 rtsp:// 地址。认证信息在下方单独填写，不放进 URL。", true));
      nodes.push(h("div", { class: "form-grid" },
        field("username", "用户名（可选）", fields.username, existing && raw.username ? "清空用户名会同时清除已保存的密码" : ""),
        field("password", "密码（可选）", fields.password, existing ? "密码不回显；填写新密码将覆盖原密码" : "有密码时需填写用户名")));
      if (existing && raw.username) {
        nodes.push(h("label", { class: "check" }, fields.clear, "清除已保存的密码（保留用户名）"));
      }
      if (!existing) nodes.push(h("p", { class: "muted tiny" }, "创建后默认未拉流，需要时点击列表中的「开始拉流」。"));
    } else if (!existing) {
      nodes.push(h("div", { class: "explainer" }, h("strong", {}, `${PUSH_PROTOCOLS[choice]} 推流接入`), h("p", {}, PUSH_HELP[choice])));
      if (prepared) {
        nodes.push(pushURLRows(pushURLs({ ...prepared, protocol: choice })),
          h("ul", { class: "rules" },
            h("li", {}, "点击「确定添加」后地址才生效，有效期 5 分钟；取消添加则地址作废。"),
            h("li", {}, "首次接入成功后地址立即失效，已建立的推流不受影响。地址可在设备详情中再次查看。")));
      } else if (prepareError) {
        nodes.push(h("div", { class: "notice warn" }, icon("alert"), h("span", {}, `接入地址生成失败：${errorMessage(prepareError)}`)));
      } else {
        nodes.push(h("p", { class: "muted tiny" }, "正在生成接入地址…"));
        void prepare();
      }
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
    if (choice === "gb" && !/^[0-9]{20}$/.test(fields.device_id.value.trim())) errors.device_id = "请输入 20 位数字的设备编号";
    if (choice === "rtsp-pull") {
      if (!validRTSPURL(fields.url.value.trim())) errors.url = "请输入 rtsp:// 开头、不含账号密码的地址";
      if (!fields.username.value && fields.password.value) errors.username = "设置密码前请输入用户名";
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
  // 提交期间 Esc 不关闭对话框。
  const onCancel = event => { if (busy) event.preventDefault(); };
  dialog.addEventListener("cancel", onCancel);
  dialog.addEventListener("close", () => dialog.removeEventListener("cancel", onCancel), { once: true });
  openDialog(dialog, existing ? fields.name : picker);
}

// 与服务端 validRTSPSourceURL 一致：rtsp 协议、有主机名、不内嵌账号密码。
function validRTSPURL(value) {
  try {
    const url = new URL(value);
    return url.protocol === "rtsp:" && Boolean(url.hostname) && !url.username && !url.password;
  } catch {
    return false;
  }
}

// 返回与实际请求一致的成功提示。
async function save(choice, existing, name, fields, prepared) {
  if (choice === "gb") {
    await api.createDevice({ device_id: fields.device_id.value.trim(), name });
    return "已添加设备，等待设备注册";
  }
  if (existing?.kind === "push" || ["rtmp", "rtsp", "whip"].includes(choice)) {
    if (existing) {
      await api.patchPushDevice(existing.id, choice === existing.raw.protocol ? { name } : { name, protocol: choice });
      return "已保存设备";
    }
    await api.createPushDevice({ name, protocol: choice, token: prepared.token });
    return `已添加 ${PUSH_PROTOCOLS[choice]} 推流设备，接入地址 5 分钟内有效`;
  }
  const payload = { stream_name: name, url: fields.url.value.trim() };
  const username = fields.username.value;
  const password = fields.password.value;
  if (!existing) {
    if (username) {
      Object.assign(payload, { username, password });
    }
    await api.createSource(payload);
    return "已添加 RTSP 拉流源";
  }
  // 沿用现有 PATCH 语义：密码留空不发送（保留）；填写则覆盖；勾选清除或清空用户名则发送空串清除。
  payload.username = username;
  if (fields.clear.checked || (!username && existing.raw.username)) {
    payload.password = "";
  } else if (password) {
    payload.password = password;
  }
  await api.patchSource(existing.id, payload);
  if (payload.password === undefined) {
    return "已保存设备";
  }
  return payload.password ? "已保存，并更新了密码" : "已保存，并清除了已保存的密码";
}
