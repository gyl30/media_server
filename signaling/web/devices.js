// 设备管理：GB28181 设备、RTSP 拉流源、推流设备统一列表，以及添加 / 编辑 / 删除。
import { api } from "/api.js";
import { store, unifiedDevices, deviceHash, refresh } from "/store.js";
import {
  h, icon, badge, protoTag, emptyState, toast, openDialog, confirmAction, errorMessage, replaceKeepingFocus,
} from "/ui.js";

const KIND_FILTERS = [["all", "全部"], ["gb", "GB28181"], ["rtsp", "RTSP 拉流"], ["push", "推流设备"]];
const filters = { query: "", kind: "all", status: "all" };

export function mountDevices(root, ctx) {
  const search = h("input", {
    class: "input", type: "search", placeholder: "搜索设备名称或 ID", "aria-label": "搜索设备名称或 ID", value: filters.query,
    oninput: event => { filters.query = event.target.value; update(); },
  });
  const segmented = h("div", { class: "segmented", role: "group", "aria-label": "按接入类型筛选" });
  const status = h("select", {
    class: "select", style: "width:140px", "aria-label": "按状态筛选",
    onchange: event => { filters.status = event.target.value; update(); },
  }, h("option", { value: "all" }, "全部状态"), h("option", { value: "online" }, "在线 / 运行中"),
  h("option", { value: "offline" }, "离线 / 未运行"), h("option", { value: "attention" }, "需关注"));
  status.value = filters.status;
  const tbody = h("tbody");
  const body = h("div");
  const foot = h("div");

  root.replaceChildren(h("div", { class: "page" },
    h("div", { class: "page-head" },
      h("div", {}, h("h1", {}, "设备管理"), h("p", {}, "GB28181 设备、RTSP 拉流源和推流设备统一管理。点击设备进入详情，查看通道、取流状态与播放地址。")),
      h("div", { class: "actions" }, h("button", { class: "btn primary", type: "button", onclick: () => openDeviceDialog() }, icon("plus"), "添加设备"))),
    h("div", { class: "card table-card" },
      h("div", { class: "toolbar" }, h("label", { class: "search" }, icon("search"), search), segmented, status),
      body, foot)));

  function update() {
    const all = unifiedDevices();
    const query = filters.query.trim().toLowerCase();
    const rows = all.filter(device => (filters.kind === "all" || device.kind === filters.kind)
      && (filters.status === "all" || device.status.group === filters.status)
      && (!query || device.name.toLowerCase().includes(query) || device.id.toLowerCase().includes(query)));

    replaceKeepingFocus(segmented, KIND_FILTERS.map(([kind, label]) => h("button", {
      type: "button", class: filters.kind === kind ? "is-active" : "", "aria-pressed": String(filters.kind === kind), dataset: { key: `kind:${kind}` },
      onclick: () => { filters.kind = kind; update(); },
    }, label, h("span", { class: "n" }, String(kind === "all" ? all.length : all.filter(device => device.kind === kind).length)))));

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
        ? emptyState({ icon: "devices", title: "还没有设备", text: "添加 GB28181 设备、RTSP 拉流源或推流设备。GB28181 设备注册后会自动上报通道。",
          action: h("button", { class: "btn primary", type: "button", onclick: () => openDeviceDialog() }, icon("plus"), "添加设备") })
        : emptyState({ icon: "search", title: "没有符合条件的设备", text: "尝试更换关键字，或清除类型、状态筛选。",
          action: h("button", { class: "btn", type: "button", onclick: () => { Object.assign(filters, { query: "", kind: "all", status: "all" }); search.value = ""; status.value = "all"; update(); } }, "清除筛选") }));
      foot.replaceChildren();
      return;
    }
    if (!body.contains(tbody)) {
      body.replaceChildren(h("div", { class: "table-scroll" }, h("table", { class: "table cards" },
        h("thead", {}, h("tr", {}, h("th", { style: "width:28%" }, "设备名称"), h("th", {}, "设备 ID"), h("th", {}, "接入类型"),
          h("th", {}, "状态"), h("th", { class: "num hide-sm" }, "通道"), h("th", { class: "actions" }, "操作"))),
        tbody)));
    }
    replaceKeepingFocus(tbody, rows.map(device => deviceRow(device, ctx)));
    const filtered = filters.kind !== "all" || filters.status !== "all" || query;
    foot.replaceChildren(h("div", { class: "table-foot" },
      h("span", {}, filtered ? `筛选出 ${rows.length} 台，共 ${all.length} 台设备` : `共 ${all.length} 台设备`),
      store.error ? h("span", { class: "stale" }, icon("alert", "sm"), "最近一次刷新失败，显示的是上次结果") : null));
  }

  update();
  return { update, unmount() {} };
}

function skeletonTable() {
  return h("div", { class: "table-scroll", "aria-busy": "true" }, h("table", { class: "table" }, h("tbody", {},
    [0, 1, 2, 3, 4].map(index => h("tr", {},
      h("td", {}, h("div", { class: "skeleton", style: `width:${60 - index * 5}%` })),
      h("td", {}, h("div", { class: "skeleton", style: "width:70%" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:64px" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:56px" })),
      h("td", {}, h("div", { class: "skeleton", style: "width:96px;margin-left:auto" })))))));
}

function deviceRow(device, ctx) {
  const hash = deviceHash(device.kind, device.id);
  const sub = device.kind === "gb" ? device.access : device.kind === "rtsp" ? `${device.access}${device.address ? ` · ${device.address}` : ""}` : device.access;
  const editable = editState(device);
  return h("tr", {},
    h("td", { class: "c-name" }, h("div", { class: "cell-title" },
      h("a", { class: "link-btn", href: hash, dataset: { key: `open:${device.kind}:${device.id}` } }, device.name || "未命名"),
      h("span", { class: "direction" }, sub))),
    h("td", { class: "c-id" }, h("span", { class: "mono", title: device.id }, device.id)),
    h("td", { class: "c-proto" }, protoTag(device.kind)),
    h("td", { class: "c-status" }, h("div", { class: "cell-title" },
      h("span", {}, badge(device.status.label, device.status.tone)),
      device.status.detail ? h("span", { class: "direction" }, device.status.detail) : null)),
    h("td", { class: "num hide-sm" }, device.kind === "gb" && device.channels ? String(device.channels.length) : h("span", { class: "muted" }, "—")),
    h("td", { class: "actions" }, h("div", { class: "actions-inner" },
      h("a", { class: "btn sm", href: hash }, "详情", icon("chevron-right", "sm")),
      h("button", {
        type: "button", class: "icon-btn", "aria-label": "编辑", title: editable.reason || "编辑", disabled: !editable.ok,
        dataset: { key: `edit:${device.kind}:${device.id}` }, onclick: () => openDeviceDialog(device),
      }, icon("edit")),
      h("button", {
        type: "button", class: "icon-btn danger", "aria-label": "删除", title: "删除", dataset: { key: `delete:${device.kind}:${device.id}` },
        onclick: () => deleteDevice(device, ctx),
      }, icon("trash")))));
}

// GB28181 没有修改接口；RTSP 源取流期间不可修改（与服务端约束一致）。
export function editState(device) {
  if (device.kind === "gb") {
    return { ok: false, reason: "GB28181 设备暂不支持编辑" };
  }
  if (device.kind === "rtsp" && device.raw.session) {
    return { ok: false, reason: "停止取流后可编辑" };
  }
  return { ok: true, reason: "" };
}

export function deleteDevice(device, ctx) {
  const messages = {
    gb: `删除「${device.name}」会停止该设备所有通道的点播，正在观看的用户都会断开。`,
    rtsp: `删除「${device.name}」会先停止取流，正在观看的用户会断开。`,
    push: `删除「${device.name}」会停止当前推流并作废已签发的推流地址，正在观看的用户会断开。`,
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

const KIND_OPTIONS = [
  ["gb", "GB28181", "设备主动注册，平台按通道点播"],
  ["rtsp", "RTSP 拉流", "平台连接摄像机或媒体服务地址"],
  ["push", "推流设备", "编码器、OBS 或 WebRTC 设备向平台推流"],
];

// existing 为空时是添加；否则编辑（RTSP 源或推流设备）。
export function openDeviceDialog(existing = null) {
  const dialog = document.getElementById("device-dialog");
  let kind = existing?.kind || "gb";
  let busy = false;
  const raw = existing?.raw;
  const fields = {
    name: h("input", { class: "input", name: "name", autocomplete: "off", value: existing?.name || "" }),
    device_id: h("input", { class: "input mono", name: "device_id", inputmode: "numeric", maxlength: "20", autocomplete: "off", placeholder: "34020000001320000001" }),
    url: h("input", { class: "input mono", name: "url", autocomplete: "off", placeholder: "rtsp://192.168.1.64:554/Streaming/Channels/101", value: raw?.url || "" }),
    username: h("input", { class: "input", name: "username", autocomplete: "off", placeholder: "可选", value: raw?.username || "" }),
    password: h("input", { class: "input", name: "password", type: "password", autocomplete: "new-password", placeholder: existing ? "留空则保留原密码" : "可选" }),
    clear: h("input", { type: "checkbox", name: "clear_password" }),
  };
  const errors = {};
  const failure = h("div", { class: "notice warn", role: "alert", hidden: true }, icon("alert"), h("span"));
  const submit = h("button", { class: "btn primary", type: "submit" }, existing ? "保存" : "添加设备");
  const cancel = h("button", { class: "btn", type: "button", onclick: () => { if (!busy) dialog.close(); } }, "取消");
  const close = h("button", { class: "icon-btn", type: "button", "aria-label": "关闭", onclick: () => { if (!busy) dialog.close(); } }, icon("x"));
  const content = h("div", { class: "dialog-body" });

  const field = (key, label, input, hint = "") => {
    const small = h("small", {}, errors[key] || hint);
    return h("label", { class: `field${errors[key] ? " invalid" : ""}` }, h("span", {}, label), input, errors[key] || hint ? small : null);
  };

  function render() {
    const picker = existing
      ? h("div", { class: "field" }, h("span", {}, "接入类型"),
        h("div", { class: "inline" }, protoTag(kind), h("span", { class: "muted" }, "接入类型创建后不可修改")))
      : h("div", { class: "field" }, h("span", {}, "接入类型"), h("div", { class: "proto-picker three", role: "radiogroup", "aria-label": "接入类型" },
        KIND_OPTIONS.map(([value, label, hint]) => h("button", {
          type: "button", role: "radio", "aria-checked": String(kind === value), class: `proto-option${kind === value ? " is-active" : ""}`,
          onclick: () => { kind = value; for (const key of Object.keys(errors)) delete errors[key]; render(); },
        }, h("strong", {}, label), h("span", {}, hint)))));
    const grid = h("div", { class: "form-grid" });
    if (kind === "gb") {
      grid.append(field("name", "设备名称", fields.name),
        field("device_id", "设备 ID", fields.device_id, "20 位 GB28181 设备编码"));
    } else if (kind === "rtsp") {
      grid.append(h("div", { class: "full" }, field("name", "源名称", fields.name)),
        h("div", { class: "full" }, field("url", "RTSP 地址", fields.url, "不要在地址中填写用户名和密码，请使用下方认证字段")),
        field("username", "用户名", fields.username, existing && raw.username ? "清空用户名会同时清除已保存的密码" : ""),
        field("password", "密码", fields.password, existing ? "密码不回显；填写新密码将覆盖原密码" : ""));
      if (existing && raw.username) {
        grid.append(h("label", { class: "full check" }, fields.clear, "清除已保存的密码（保留用户名）"));
      }
    } else {
      grid.append(h("div", { class: "full" }, field("name", "设备名称", fields.name)));
    }
    const notes = {
      gb: "保存后，设备使用该 ID 向平台注册；注册成功后通道自动上报。平台不会因此自动点播。",
      rtsp: "保存后不会立即连接摄像机。在设备详情中点击「开始取流」后，平台才建立 RTSP 拉流。",
      push: "在设备详情中签发推流地址：一次签发同时提供 RTMP、RTSP、WHIP 三种地址，共用一个一次性授权，需在有效期内首次连接。当前服务端不支持为设备固定推流协议或长期接入地址。",
    };
    content.replaceChildren(picker, grid, h("div", { class: "notice info" }, icon("info"), h("span", {}, notes[kind])), failure);
  }

  const syncPassword = () => {
    const cleared = fields.clear.checked || (existing && raw?.username && !fields.username.value);
    fields.password.disabled = Boolean(cleared);
    if (cleared) {
      fields.password.value = "";
    }
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
    if (kind === "gb" && !/^[0-9]{20}$/.test(fields.device_id.value.trim())) errors.device_id = "请输入 20 位数字的设备编码";
    if (kind === "rtsp") {
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
      const message = await save(kind, existing, name, fields);
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

  const form = h("form", { novalidate: true, onsubmit: onSubmit },
    h("div", { class: "dialog-head" }, h("h2", {}, existing ? "编辑设备" : "添加设备"), close),
    content,
    h("div", { class: "dialog-foot" }, cancel, submit));
  dialog.replaceChildren(form);
  dialog.setAttribute("aria-label", existing ? "编辑设备" : "添加设备");
  render();
  // 提交期间 Esc 不关闭对话框。
  const onCancel = event => { if (busy) event.preventDefault(); };
  dialog.addEventListener("cancel", onCancel);
  dialog.addEventListener("close", () => dialog.removeEventListener("cancel", onCancel), { once: true });
  openDialog(dialog, fields.name);
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
async function save(kind, existing, name, fields) {
  if (kind === "gb") {
    await api.createDevice({ device_id: fields.device_id.value.trim(), name });
    return "已添加设备，等待设备注册";
  }
  if (kind === "push") {
    if (existing) {
      await api.patchPushDevice(existing.id, { name });
      return "已保存设备";
    }
    await api.createPushDevice({ name });
    return "已添加推流设备";
  }
  const payload = { stream_name: name, url: fields.url.value.trim() };
  const username = fields.username.value;
  const password = fields.password.value;
  if (!existing) {
    if (username) {
      Object.assign(payload, { username, password });
    }
    await api.createSource(payload);
    return "已添加 RTSP 源";
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
