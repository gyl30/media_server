// 设备详情的播放地址面板。按当前服务端语义：POST /api/play 一次签发 5 个 URL，共用一个一次性 token
// 和同一个 expires_at；任一地址被使用后其余地址随之失效，前端无法得知是否已被使用。
import { api } from "/api.js";
import { h, icon, badge, emptyState, toast, errorMessage, copyButton, replaceKeepingFocus } from "/ui.js";

const PROTOCOLS = [["RTSP", "rtsp_url"], ["RTMP", "rtmp_url"], ["HTTP-FLV", "flv_url"], ["HLS", "hls_url"], ["WHEP", "whep_url"]];

export function createPlaybackPanel() {
  let stream = null;
  let group = null;
  let issuing = false;
  let failure = null;
  let ticket = 0;
  let controller = null;
  let title = "";
  let notReady = null;
  let actions = [];
  const head = h("div", { class: "card-head" });
  const body = h("div");
  const element = h("section", { class: "card urls-panel", "aria-label": "播放地址" }, head, body);
  const timer = window.setInterval(tick, 1000);

  // 只接受最新一次申请、且属于当前媒体流的结果。
  async function issue(manual) {
    const current = ++ticket;
    controller?.abort();
    controller = new AbortController();
    const expected = stream;
    issuing = true;
    failure = null;
    render();
    try {
      const result = await api.play(expected.source, controller.signal);
      if (current !== ticket || stream?.id !== expected.id || result.stream_id !== expected.id) {
        return;
      }
      group = result;
      if (manual) toast("已重新申请播放地址", "success");
    } catch (error) {
      if (current !== ticket || error.name === "AbortError") {
        return;
      }
      group = null;
      failure = error;
    } finally {
      if (current === ticket) {
        issuing = false;
        controller = null;
        render();
      }
    }
  }

  function cancel() {
    ticket += 1;
    controller?.abort();
    controller = null;
    issuing = false;
  }

  // next.stream 为空表示媒体未就绪，此时显示 next.notReady，不申请地址。
  function update(next) {
    title = next.title;
    notReady = next.notReady;
    actions = next.actions || [];
    if (stream?.id !== next.stream?.id) {
      cancel();
      stream = next.stream;
      group = null;
      failure = null;
      if (stream) {
        void issue(false);
        return;
      }
    }
    // 同一媒体流的轮询只刷新标题区，不重建地址行（保留复制反馈与焦点）。
    if (stream) {
      renderHead();
      return;
    }
    render();
  }

  function expired() {
    return Boolean(group) && Date.parse(group.expires_at) <= Date.now();
  }

  function secondsLeft() {
    return Math.max(0, Math.ceil((Date.parse(group.expires_at) - Date.now()) / 1000));
  }

  function tick() {
    if (!group) {
      return;
    }
    const left = secondsLeft();
    const counter = body.querySelector("[data-countdown]");
    if (counter) {
      counter.textContent = `${left}s`;
      counter.closest(".group-ttl")?.classList.toggle("low", left <= 10);
    }
    if (left === 0 && !body.querySelector("[data-expired]")) {
      render();
    }
  }

  function renderHead() {
    replaceKeepingFocus(head, [
      h("div", { style: "min-width:0" }, h("h2", {}, "播放地址"), h("div", { class: "sub ellipsis" }, title)),
      h("div", { class: "head-actions" },
        h("button", { class: "btn sm", type: "button", disabled: !stream || issuing, "aria-busy": String(issuing && Boolean(stream)), dataset: { key: "refresh-urls" },
          onclick: () => void issue(true) }, issuing ? null : icon("refresh", "sm"), "全部刷新"),
        actions)]);
  }

  function render() {
    renderHead();
    if (!stream) {
      body.replaceChildren(notReady);
      return;
    }
    if (failure) {
      body.replaceChildren(emptyState({
        icon: "alert", tone: "danger", title: "播放地址申请失败", text: `${errorMessage(failure)}。已建立的播放不受影响。`,
        action: h("button", { class: "btn", type: "button", onclick: () => void issue(true) }, icon("refresh"), "重新申请"),
      }));
      return;
    }
    const dead = expired();
    const meta = h("div", { class: "urls-meta" },
      h("div", { class: "stream" }, h("span", {}, "媒体流"), h("strong", { class: "mono", title: stream.id }, stream.id)),
      issuing || !group ? h("span", { class: "muted small" }, "正在申请…")
        : dead ? badge("已过期", "warn")
        : h("div", { class: `group-ttl${secondsLeft() <= 10 ? " low" : ""}`, role: "timer" }, icon("clock", "sm"), h("span", { class: "ttl", "data-countdown": "" }, `${secondsLeft()}s`), h("span", {}, "内可发起新连接")));
    const rows = PROTOCOLS.map(([label, key]) => {
      const url = group?.[key] || "";
      return h("div", { class: `url-row${dead ? " expired" : ""}` },
        h("div", { class: "url-proto" }, h("strong", {}, label)),
        h("div", { class: "url-side" }, url && !issuing ? copyButton(url, ` ${label} 播放地址`) : null),
        issuing || !group ? h("div", { class: "skeleton url-skeleton" }) : h("div", { class: "url-value" }, url));
    });
    rows.forEach(row => { if (dead) row.querySelector(".copy-btn")?.setAttribute("disabled", ""); });
    body.replaceChildren(...[meta,
      dead ? h("div", { class: "notice warn group-notice", "data-expired": "" }, icon("clock"),
        h("span", { style: "flex:1" }, "本组地址已过期，不能再用于新的播放连接；已建立的播放不受影响。"),
        h("button", { class: "btn xs", type: "button", onclick: () => void issue(true) }, "重新申请")) : null,
      h("div", { class: "url-rows", "aria-busy": String(issuing) }, rows),
      h("div", { class: "urls-foot" }, h("ul", { class: "rules" },
        h("li", {}, "五个地址共用同一个一次性授权：任一地址被播放器使用后，其余地址随之失效。"),
        h("li", {}, "倒计时只表示尚未过期，不能说明地址未被使用；需要给多个播放器使用时，请为每个播放器重新申请。"),
        h("li", {}, "已建立的播放不会因过期而中断。每次打开详情都会重新申请，也可以随时「全部刷新」。")))].filter(Boolean));
  }

  return {
    element,
    update,
    destroy() {
      cancel();
      window.clearInterval(timer);
    },
  };
}
