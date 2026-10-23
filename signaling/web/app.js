import { store, subscribe, refresh, refreshing, stopRefresh, loadChannels } from "/store.js";
import { h, icon, toast, errorMessage } from "/ui.js";
import { mountDevices } from "/devices.js";
import { mountDetail } from "/detail.js";
import { mountVideo } from "/monitor.js";
import { mountOverview } from "/overview.js";

const PAGES = [
  { page: "overview", label: "概览", icon: "overview", mount: mountOverview },
  { page: "devices", label: "设备管理", icon: "devices", mount: mountDevices },
  { page: "monitor", label: "实时监控", icon: "video", mount: mountVideo },
];

const view = document.getElementById("view");
const nav = document.getElementById("nav");
const crumbs = document.getElementById("crumbs");
const shell = document.getElementById("shell");
const menu = document.getElementById("menu-button");
const refreshButton = document.getElementById("refresh-button");
const connection = document.getElementById("connection");
let current = null;
let route = null;

const ctx = {
  pendingPlay: null,
  playInCenter(item) {
    ctx.pendingPlay = item;
    location.hash = "#monitor";
  },
  setTitle(title) {
    if (route?.id) {
      crumbs.replaceChildren(h("a", { href: "#devices" }, "设备管理"), icon("chevron-right", "sm"), h("h1", {}, title));
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
    class: `nav-item${route.page === item.page ? " is-active" : ""}`, href: `#${item.page}`,
    "aria-current": route.page === item.page ? "page" : undefined, onclick: () => setNav(false),
  }, icon(item.icon), item.label)));
}

function renderConnection() {
  connection.replaceChildren(...(store.error ? [icon("alert", "sm"), "连接中断"] : []));
  connection.title = store.error ? `${errorMessage(store.error)}，页面数据可能不是最新` : "";
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
  crumbs.replaceChildren(route.id ? h("a", { href: "#devices" }, page.label) : h("h1", {}, page.label));
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
  refreshButton.disabled = false;
  refreshButton.removeAttribute("aria-busy");
  renderConnection();
  current?.update();
});

refreshButton.addEventListener("click", async () => {
  refreshButton.disabled = true;
  refreshButton.setAttribute("aria-busy", "true");
  await Promise.all([refresh(), loadChannels([...store.channels.keys()])]);
  if (store.error) toast(errorMessage(store.error), "danger");
});
menu.addEventListener("click", () => {
  setNav(!shell.classList.contains("nav-open"));
  if (shell.classList.contains("nav-open")) {
    nav.querySelector(".is-active")?.focus();
  }
});
document.addEventListener("keydown", event => {
  if (event.key === "Escape" && shell.classList.contains("nav-open")) {
    setNav(false);
    menu.focus();
  }
});
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
