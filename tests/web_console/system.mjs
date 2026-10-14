// 系统总览 / 媒体流 / 运行状态：数值与接口独立计算的结果一致；首次加载的加载中与失败；刷新失败提示；无虚构指标。
import { base, shots, check, clickText, text, api, waitFor, summary } from "./lib.mjs";

async function expected() {
  const devices = (await api("/api/devices")).body.devices;
  const sources = (await api("/api/sources")).body.sources;
  const pushes = (await api("/api/push-devices")).body.devices;
  let lives = 0;
  for (const device of devices.filter(item => item.online)) {
    lives += (await api(`/api/devices/${device.device_id}/channels`)).body.channels.filter(channel => channel.live?.state === "streaming").length;
  }
  const pulls = sources.filter(source => source.session?.state === "created").length;
  const pushing = pushes.filter(device => device.state === "verified").length;
  return {
    total: devices.length + sources.length + pushes.length,
    online: devices.filter(item => item.online).length + sources.filter(source => ["created", "starting", "stopping"].includes(source.session?.state)).length + pushing,
    streams: lives + pulls + pushing, lives, pulls, pushing,
  };
}

export default async function (b) {
  const q = js => b.eval(js);
  await b.viewport(1440, 900);
  await b.goto(base + "#overview", 3000);
  const want = await expected();
  const kpi = index => q(`document.querySelectorAll('.kpi .value')[${index}].childNodes[0].textContent`);
  check("已登记设备数与接口一致", Number(await kpi(0)) === want.total, `${await kpi(0)} vs ${want.total}`);
  check("在线 / 运行中与接口一致", Number(await kpi(1)) === want.online, `${await kpi(1)} vs ${want.online}`);
  check("运行中媒体流与接口一致", Number(await kpi(2)) === want.streams, `${await kpi(2)} vs ${want.streams}`);
  check("媒体流构成", (await q(text(".kpis"))).includes(`GB 点播 ${want.lives} · RTSP 拉流 ${want.pulls} · 推流 ${want.pushing}`));
  check("总览没有虚构图表或指标", !/(CPU|Mbps|%)/.test(await q(text("#view"))));
  await b.shot(`${shots}/overview.png`);

  // 媒体流
  await q("location.hash = '#streams'");
  await b.sleep(800);
  check("媒体流行数与接口一致", await q("document.querySelectorAll('.table tbody tr').length") === want.streams);
  check("不展示轨道 / 观看数等虚构列", !(await q(text(".table thead"))).includes("观看"));
  await b.shot(`${shots}/streams.png`);
  if (want.streams > 0) {
    await q("document.querySelector('.table tbody tr .btn').click()");
    check("从媒体流播放：视频中心只开这一路", await waitFor(b, "location.hash === '#video' && document.querySelectorAll('.tile.is-streaming').length === 1", 15000));
    await q("location.hash = '#status'");
    await b.sleep(800);
    check("离开后无视频元素", await q("document.querySelectorAll('video').length") === 0);
  }

  // 运行状态
  await q("location.hash = '#status'");
  await b.sleep(800);
  const status = await q(text("#view"));
  check("运行状态显示接口正常与数据时间", status.includes("正常") && status.includes("数据时间"));
  check("资源指标标为暂不支持且无数值", status.includes("暂不支持") && !/\d+\s*(%|MB|Mbps)/.test(status));
  await b.shot(`${shots}/status.png`);

  // 刷新失败：保留上次数据并提示
  await q("window.__fail = true; const original = window.fetch; window.fetch = (url, options) => window.__fail && String(url).startsWith('/api/') ? Promise.reject(new TypeError('Failed to fetch')) : original(url, options)");
  await q(clickText(".topbar .btn", "刷新"));
  await b.sleep(600);
  check("刷新失败提示", (await q(text("#toasts"))).includes("无法连接服务器"));
  check("运行状态显示最近一次刷新失败", (await q(text("#view"))).includes("最近一次刷新失败"));
  await q("location.hash = '#devices'");
  await b.sleep(500);
  check("设备列表保留上次结果并提示", (await q(text(".table-foot"))).includes("最近一次刷新失败") && await q("document.querySelectorAll('.table tbody tr').length") === want.total);
  await q("window.__fail = false");

  // 首次加载：加载中骨架与失败重试
  const slow = await b.beforeLoad("{ const original = window.fetch; window.fetch = (url, options) => String(url).startsWith('/api/') ? new Promise(resolve => setTimeout(() => resolve(original(url, options)), 3000)) : original(url, options); }");
  await b.goto(base + "#devices", 0);
  await b.reload(800);
  check("首次加载显示骨架", await q("!!document.querySelector('.table-card .skeleton')"));
  await b.shot(`${shots}/devices-loading.png`);
  await b.removeBeforeLoad(slow);
  const fail = await b.beforeLoad("{ const original = window.fetch; window.fetch = (url, options) => String(url).startsWith('/api/') && !window.__allow ? Promise.reject(new TypeError('Failed to fetch')) : original(url, options); }");
  await b.reload(1500);
  check("首次加载失败显示错误与重试", (await q(text(".table-card"))).includes("设备列表暂时无法加载"));
  await b.shot(`${shots}/devices-error.png`);
  await q("window.__allow = true");
  await q(clickText(".table-card .btn", "重试"));
  check("重试后加载成功", await waitFor(b, `document.querySelectorAll('.table tbody tr').length === ${want.total}`, 5000));
  await b.removeBeforeLoad(fail);
  summary();
}
