// GB 通道请求持续失败时不能形成请求循环：每个页面在一个轮询周期内对同一设备最多请求一次；
// 失败后由下一次轮询或手动刷新重试，接口恢复后通道正常显示。
import { base, check, clickText, text, waitFor, summary } from "./lib.mjs";

const GB = "34020000001320000101";
// 文档加载前注入：统计通道请求，window.__channelFail 为真时让通道请求失败。
const inject = `{
  window.__channelRequests = 0; window.__channelFail = true;
  const original = window.fetch;
  window.fetch = (url, options) => {
    if (String(url).endsWith('/channels')) {
      window.__channelRequests += 1;
      if (window.__channelFail) return Promise.resolve(new Response(JSON.stringify({ error: 'device_store_failed' }), { status: 500, headers: { 'Content-Type': 'application/json' } }));
    }
    return original(url, options);
  };
}`;

export default async function (b) {
  const q = js => b.eval(js);
  const countDuring = async ms => { const start = await q("window.__channelRequests"); await b.sleep(ms); return (await q("window.__channelRequests")) - start; };
  await b.viewport(1440, 900);
  const id = await b.beforeLoad(inject);

  await b.goto(base + `#devices/gb/${GB}`, 1500);
  const detail = await countDuring(3000);
  check("详情页：通道持续失败时不循环请求（3 秒内至多 2 次）", detail <= 2, String(detail));
  check("详情页：失败时提示通道加载失败", (await q(text(".page"))).includes("通道加载失败"));

  await q("location.hash = '#devices'");
  await b.sleep(500);
  const list = await countDuring(3000);
  check("设备列表：通道持续失败时不循环请求", list <= 2, String(list));

  await q("location.hash = '#monitor'");
  await b.sleep(1000);
  await q(`document.querySelector('[data-key="toggle:${GB}"]').click()`);
  const monitor = await countDuring(3000);
  check("实时监控展开：通道持续失败时不循环请求", monitor <= 2, String(monitor));
  check("实时监控展开：提示通道加载失败", (await q(text(".directory"))).includes("通道加载失败"));
  await q(`document.querySelector('[data-key="toggle:${GB}"]').click()`);
  check("收起后不再请求", (await countDuring(3000)) === 0);

  // 接口恢复：下一次轮询或手动刷新即可恢复
  await q("window.__channelFail = false");
  await q(`document.querySelector('[data-key="toggle:${GB}"]').click()`);
  check("恢复后展开显示通道", await waitFor(b, "document.querySelector('.directory').innerText.includes('simulator camera')", 6000));
  await q(`location.hash = '#devices/gb/${GB}'`);
  check("恢复后详情显示通道", await waitFor(b, "document.querySelector('.channel-list')?.innerText.includes('simulator camera')", 6000));
  await q("window.__channelFail = true");
  await q(clickText(".topbar .btn", "刷新"));
  await b.sleep(800);
  check("手动刷新仍会请求通道，失败时保留上次结果", (await q(text(".channel-list"))).includes("simulator camera"));
  await b.removeBeforeLoad(id);
  summary();
}
