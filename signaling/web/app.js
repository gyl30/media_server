// 应用外壳：hash 路由、侧边栏、快照轮询。每个页面 mount 后返回 { update, unmount }，
// 路由变化（侧边栏、前进 / 后退、手改 hash）统一经过 hashchange，旧页面先 unmount 再挂载新页面。
import { store, subscribe, refresh, refreshing, stopRefresh, unifiedDevices, runningStreams } from "/store.js";
import { h, icon, toast, formatClock, errorMessage } from "/ui.js";
import { mountDevices } from "/devices.js";
import { mountDetail } from "/detail.js";
import { mountVideo } from "/video.js";
import { mountOverview, mountStreams, mountStatus } from "/system.js";

const PAGES = [
  { page: "overview", label: "系统总览", icon: "overview", mount: mountOverview },
  { page: "devices", label: "设备管理", icon: "devices", mount: mountDevices, count: () => unifiedDevices().length },
  { page: "video", label: "视频中心", icon: "video", mount: mountVideo },
  { page: "streams", label: "媒体流", icon: "streams", mount: mountStreams, count: () => runningStreams().length },
  { page: "status", label: "运行状态", icon: "activity", mount: mountStatus },
];

const view = document.getElementById("view");
const nav = document.getElementById("nav");
const crumbs = document.getElementById("crumbs");
const shell = document.getElementById("shell");
const menu = document.getElementById("menu-button");
const refreshButton = document.getElementById("refresh-button");
const updated = document.getElementById("updated");
let current = null;
let route = null;

const ctx = {
  // 由详情页等入口带到视频中心的一路播放；视频中心挂载时取走。
  pendingPlay: null,
  playInCenter(item) {
    ctx.pendingPlay = item;
    location.hash = "#video";
  },
  setTitle(title) {
    if (route?.id) {
      crumbs.replaceChildren(h("a", { href: "#devices" }, icon("arrow-left", "sm"), h("span", { class: "crumb-text" }, "设备管理")),
        icon("chevron-right", "sm"), h("strong", {}, title));
    }
  },
  onDeleted(device) {
    if (route?.kind === device.kind && route?.id === device.id) {
      location.hash = "#devices";
    }
  },
};

function parseRoute() {
  const [page, kind, id] = location.hash.replace(/^#\/?/, "").split("/");
  const known = PAGES.find(item => item.page === page) || PAGES[0];
  return { page: known.page, kind: known.page === "devices" ? kind || "" : "", id: known.page === "devices" && id ? decodeURIComponent(id) : "" };
}

function renderNav() {
  nav.replaceChildren(...PAGES.map(item => h("a", {
    class: `nav-item${route.page === item.page ? " is-active" : ""}`, href: `#${item.page}`, title: item.label,
    "aria-current": route.page === item.page ? "page" : undefined, onclick: () => setNav(false),
  }, icon(item.icon), h("span", { class: "label" }, item.label), item.count && store.loaded ? h("span", { class: "count" }, String(item.count())) : null)));
}

function navigate() {
  const next = parseRoute();
  if (route && next.page === route.page && next.kind === route.kind && next.id === route.id) {
    return;
  }
  current?.unmount();
  current = null;
  route = next;
  const page = PAGES.find(item => item.page === route.page);
  crumbs.replaceChildren(h("strong", {}, page.label));
  view.replaceChildren();
  current = route.id ? mountDetail(view, ctx, route.kind, route.id) : page.mount(view, ctx);
  renderNav();
  window.scrollTo(0, 0);
}

function setNav(open) {
  shell.classList.toggle("nav-open", open);
  menu.setAttribute("aria-expanded", String(open));
}

subscribe(() => {
  updated.textContent = store.updatedAt ? `更新于 ${formatClock(store.updatedAt)}` : "尚未刷新";
  refreshButton.disabled = false;
  refreshButton.removeAttribute("aria-busy");
  renderNav();
  current?.update();
});

refreshButton.addEventListener("click", async () => {
  refreshButton.disabled = true;
  refreshButton.setAttribute("aria-busy", "true");
  await refresh();
  if (store.error) toast(errorMessage(store.error), "danger");
  else toast("已刷新", "success");
});
menu.addEventListener("click", () => setNav(!shell.classList.contains("nav-open")));
window.addEventListener("hashchange", () => { setNav(false); navigate(); });

const polling = window.setInterval(() => {
  if (!refreshing()) void refresh();
}, 2500);
window.addEventListener("pagehide", () => {
  clearInterval(polling);
  stopRefresh();
});

navigate();
void refresh();
