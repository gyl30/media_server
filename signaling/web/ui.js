// 各页面共用的 DOM 构造、提示、确认框、格式化与错误文案。不持有业务状态。

// h("button", { class: "btn", onclick }, "文字", node, [nodes], null)
export function h(tag, props = {}, ...children) {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (value === undefined || value === null || value === false) {
      continue;
    }
    if (key.startsWith("on") && typeof value === "function") {
      element.addEventListener(key.slice(2), value);
    } else if (key === "class") {
      element.className = value;
    } else if (key === "dataset") {
      Object.assign(element.dataset, value);
    } else if (key in element && typeof value !== "string") {
      element[key] = value;
    } else {
      element.setAttribute(key, value === true ? "" : value);
    }
  }
  element.append(...children.flat(Infinity).filter(child => child !== null && child !== undefined && child !== false));
  return element;
}

export function icon(name, size = "") {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", `icon${size ? ` ${size}` : ""}`);
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", `/icons.svg#${name}`);
  svg.append(use);
  return svg;
}

export function badge(label, tone = "neutral", dot = true) {
  return h("span", { class: `badge ${tone}` }, dot ? h("span", { class: "dot" }) : null, label);
}

export function protoTag(kind) {
  const labels = { gb: "GB28181", rtsp: "RTSP", push: "推流" };
  return h("span", { class: `proto ${kind}`, title: kind === "push" ? "推流设备：协议未指定，签发时同时提供 RTMP / RTSP / WHIP 地址" : "" }, labels[kind]);
}

export function emptyState({ icon: glyph = "info", title, text = "", tone = "", action = null }) {
  return h("div", { class: `empty${tone ? ` ${tone}` : ""}`, role: "status" },
    h("span", { class: "glyph" }, icon(glyph, "lg")),
    h("strong", {}, title),
    text ? h("p", {}, text) : null,
    action);
}

const toastBox = () => document.getElementById("toasts");

export function toast(message, tone = "info") {
  const glyph = tone === "danger" ? "alert" : tone === "success" ? "check" : "info";
  const item = h("div", { class: `toast ${tone}` }, icon(glyph), h("span", {}, message));
  const box = toastBox();
  while (box.children.length >= 4) {
    box.firstElementChild.remove();
  }
  box.append(item);
  window.setTimeout(() => item.remove(), 3200);
}

// 打开时聚焦；关闭后焦点回到触发元素。busy 期间禁止 Esc 关闭。
export function openDialog(dialog, initialFocus) {
  const trigger = document.activeElement;
  dialog.addEventListener("close", () => {
    if (trigger?.isConnected) {
      trigger.focus({ preventScroll: true });
    }
  }, { once: true });
  dialog.showModal();
  initialFocus?.focus();
}

const confirmDialog = () => document.getElementById("confirm-dialog");

// 危险操作确认。action 抛错时保留确认框并显示原因，可直接重试；成功后关闭。
export function confirmAction({ title, message, confirmLabel, action }) {
  const dialog = confirmDialog();
  const button = dialog.querySelector("[data-confirm]");
  const cancel = dialog.querySelector("[data-cancel]");
  const error = dialog.querySelector("[data-error]");
  let busy = false;
  dialog.querySelector("h2").textContent = title;
  dialog.querySelector("[data-message]").textContent = message;
  button.textContent = confirmLabel;
  error.hidden = true;
  const setBusy = value => {
    busy = value;
    button.disabled = value;
    cancel.disabled = value;
    button.setAttribute("aria-busy", String(value));
  };
  setBusy(false);
  const onConfirm = async () => {
    if (busy) {
      return;
    }
    setBusy(true);
    error.hidden = true;
    try {
      await action();
    } catch (failure) {
      setBusy(false);
      error.querySelector("span").textContent = errorMessage(failure);
      error.hidden = false;
      button.textContent = "重试";
      return;
    }
    setBusy(false);
    dialog.close();
  };
  const onCancel = () => {
    if (!busy) {
      dialog.close();
    }
  };
  const onKey = event => {
    if (event.key === "Escape" && busy) {
      event.preventDefault();
    }
  };
  button.addEventListener("click", onConfirm);
  cancel.addEventListener("click", onCancel);
  dialog.addEventListener("cancel", onKey);
  dialog.addEventListener("close", () => {
    button.removeEventListener("click", onConfirm);
    cancel.removeEventListener("click", onCancel);
    dialog.removeEventListener("cancel", onKey);
  }, { once: true });
  openDialog(dialog, cancel);
}

export function errorMessage(error) {
  console.debug("管理操作失败", error);
  const messages = {
    device_not_found: "设备不存在", device_exists: "设备已存在", device_offline: "设备当前离线",
    device_stopping: "设备正在清理，请稍后重试", channel_not_found: "通道不存在", channel_offline: "通道当前不可播放",
    play_not_found: "播放链接已失效，请重新播放", live_not_found: "点播已结束", live_stopping: "点播正在停止，请稍后重试",
    device_delete_failed: "设备暂时无法删除，媒体资源清理未完成，请稍后重试",
    device_store_failed: "设备数据暂时无法保存，请稍后重试",
    live_stop_failed: "点播暂时无法停止，请稍后重试", live_start_failed: "设备暂时无法连接，请稍后重试",
    invalid_request: "输入有误，请检查后重试", conflict: "资源正在使用，请稍后重试", not_running: "媒体流未运行",
    whep_create_404: "播放链接已失效，请重新播放", whep_create_409: "媒体尚未就绪，请稍后重新播放",
    whep_create_403: "播放授权已失效，请重新播放", source_not_running: "媒体流未运行",
    source_start_failed: "无法开始取流，请稍后重试", source_stop_failed: "暂时无法停止取流，请稍后重试",
    source_delete_failed: "暂时无法删除 RTSP 源，请稍后重试",
    not_found: "资源不存在或已被删除", operation_failed: "操作失败，请稍后重试",
    push_device_not_found: "推流设备不存在", push_stop_failed: "暂时无法停止推流，请稍后重试",
    push_delete_failed: "暂时无法删除推流设备，请稍后重试",
    media_ended: "媒体已结束", whep_delete_500: "画面已关闭，服务器资源尚待清理",
    webrtc_connection_failed: "连接中断，请重新播放", network_error: "无法连接服务器，请稍后重试",
    invalid_whep_response: "播放服务返回异常，请重新播放", server_shutdown: "服务正在关闭",
  };
  if (messages[error?.code]) {
    return messages[error.code];
  }
  if (error?.name === "AbortError") {
    return "操作已取消";
  }
  return error instanceof TypeError ? "无法连接服务器，请检查网络后重试" : "操作失败，请稍后重试";
}

// 非安全来源（HTTP 访问内网地址）没有 navigator.clipboard，退回到选区复制。
export async function copyText(value) {
  try {
    await navigator.clipboard.writeText(value);
    return true;
  } catch {
    const area = h("textarea", { readonly: true, style: "position:fixed;opacity:0" });
    area.value = value;
    document.body.append(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function formatDuration(ms) {
  const total = Math.max(0, Math.floor(ms / 1000));
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const seconds = total % 60;
  if (hours > 0) {
    return `${hours} 小时 ${minutes} 分`;
  }
  return minutes > 0 ? `${minutes} 分 ${String(seconds).padStart(2, "0")} 秒` : `${seconds} 秒`;
}

export function formatAgo(timestamp) {
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
  if (seconds < 60) {
    return `${seconds} 秒前`;
  }
  if (seconds < 3600) {
    return `${Math.floor(seconds / 60)} 分钟前`;
  }
  return seconds < 86400 ? `${Math.floor(seconds / 3600)} 小时前` : `${Math.floor(seconds / 86400)} 天前`;
}

export function formatClock(timestamp) {
  return new Date(timestamp).toLocaleTimeString("zh-CN", { hour12: false });
}

// 轮询会重建列表区域；用 data-key 把键盘焦点还给重建后的同一个控件。
export function replaceKeepingFocus(container, nodes) {
  const active = container.contains(document.activeElement) ? document.activeElement : null;
  const key = active?.dataset.key;
  container.replaceChildren(...nodes);
  if (key) {
    container.querySelector(`[data-key="${CSS.escape(key)}"]`)?.focus({ preventScroll: true });
  }
}
