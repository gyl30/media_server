// 应用外壳：hash 路由、侧边栏、快照轮询。每个页面 mount 后返回 { update, unmount }，
// 路由变化（侧边栏、前进 / 后退、手改 hash）统一经过 hashchange，旧页面先 unmount 再挂载新页面。
import { store, subscribe, refresh, refreshing, stopRefresh, unifiedDevices, loadChannels } from "/store.js";
import { h, icon, toast, formatClock, errorMessage } from "/ui.js";
import { mountDevices } from "/devices.js";
import { mountDetail } from "/detail.js";
import { mountVideo } from "/video.js";
import { mountOverview, mountStreams, mountStatus } from "/system.js";

const PAGES = [
  { page: "overview", label: "控制台", icon: "layout", group: "工作空间", mount: mountOverview },
  { page: "devices", label: "设备管理", icon: "camera", group: "工作空间", mount: mountDevices, count: () => unifiedDevices().length },
  { page: "monitor", label: "实时监控", icon: "video", group: "工作空间", mount: mountVideo },
  { page: "resources", label: "视频资源", icon: "film", group: "平台管理", mount: mountStreams },
  { page: "status", label: "系统状态", icon: "server", group: "平台管理", mount: mountStatus },
];

const view = document.getElementById("view");
const nav = document.getElementById("nav");
const crumbs = document.getElementById("crumbs");
const shell = document.getElementById("shell");
const menu = document.getElementById("menu-button");
const refreshButton = document.getElementById("refresh-button");
const updated = document.getElementById("updated");
const serverStatus = document.getElementById("server-status");
let current = null;
let route = null;

const ctx = {
  // 由其他页面带到实时监控的一路播放；监控页挂载时取走。
  pendingPlay: null,
  playInCenter(item) {
    ctx.pendingPlay = item;
    location.hash = "#monitor";
  },
  // 从设备列表"通道"进入实时监控并展开该 GB 设备。
  monitorDevice: "",
  openMonitorDevice(deviceID) {
    ctx.monitorDevice = deviceID;
    location.hash = "#monitor";
  },
  setTitle(title) {
    if (route?.id) {
      crumbs.replaceChildren(h("a", { href: "#devices" }, "设备管理"), icon("chevron-right", "sm"), h("strong", {}, title));
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
  const nodes = [];
  let group = "";
  for (const item of PAGES) {
    if (item.group !== group) {
      if (group) nodes.push(h("div", { class: "nav-line" }));
      group = item.group;
      nodes.push(h("div", { class: "nav-kicker" }, group));
    }
    nodes.push(h("a", {
      class: `nav-item${route.page === item.page ? " is-active" : ""}`, href: `#${item.page}`, title: item.label,
      "aria-current": route.page === item.page ? "page" : undefined, onclick: () => setNav(false),
    }, icon(item.icon), h("span", { class: "label" }, item.label), item.count && store.loaded ? h("span", { class: "count" }, String(item.count())) : null));
  }
  nav.replaceChildren(...nodes);
}

function renderServerStatus() {
  const failed = Boolean(store.error);
  serverStatus.replaceChildren(h("span", { class: `dot${failed ? " err" : store.loaded ? " on" : ""}` }),
    h("div", {}, h("strong", {}, failed ? "signaling 接口异常" : "signaling 接口"),
      h("small", {}, failed ? errorMessage(store.error) : store.updatedAt ? `数据时间 ${formatClock(store.updatedAt)}` : "正在连接…")));
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
  renderServerStatus();
  current?.update();
});

// 手动刷新同时更新已缓存的 GB 通道；自动轮询只刷新三个列表。
refreshButton.addEventListener("click", async () => {
  refreshButton.disabled = true;
  refreshButton.setAttribute("aria-busy", "true");
  await Promise.all([refresh(), loadChannels([...store.channels.keys()])]);
  if (store.error) toast(errorMessage(store.error), "danger");
  else toast("已刷新", "success");
});
menu.addEventListener("click", () => setNav(!shell.classList.contains("nav-open")));
document.getElementById("shade").addEventListener("click", () => setNav(false));
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
