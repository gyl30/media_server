// 真实剪贴板（不替换 navigator.clipboard）：预生成推流接入地址读回，取消不创建有效设备。
import { base, check, clickText, api, waitFor, summary } from "./lib.mjs";
export default async function (b) {
  const q = js => b.eval(js);
  await b.viewport(1440, 900);
  await b.allowClipboard(new URL(base).origin);
  await b.goto(base + "#devices", 2200);
  try {
    await q(clickText(".page-head .btn", "添加设备"));
    await q("const s = document.querySelector('#device-dialog select[name=protocol]'); s.value = 'rtmp'; s.dispatchEvent(new Event('change', { bubbles: true }))");
    check("预生成一个真实 RTMP 接入地址", await waitFor(b, "document.querySelectorAll('#device-dialog .url-value').length === 1", 5000));
    const want = await q("document.querySelector('#device-dialog .url-value').textContent");
    await q("document.querySelector('#device-dialog .copy-btn').click()");
    await b.sleep(300);
    const got = await q("navigator.clipboard.readText()");
    check("真实剪贴板内容等于预生成接入地址", got === want);
    check("复制按钮反馈", (await q("document.querySelector('#device-dialog .copy-btn').textContent")).includes("已复制"));
    await q("document.getElementById('device-dialog').close()");
    const token = want.split("/").pop();
    check("取消复制预览不创建设备", !(await api("/api/push-devices")).body.devices.some(item => item.token === token));
  } finally {
    await q("document.getElementById('device-dialog').close()");
  }
  summary();
}
