// 真实剪贴板（不替换 navigator.clipboard）：复制播放地址后从系统剪贴板读回。需要 PUSH_DEVICE（正在推流）。
import { base, check, waitFor, summary } from "./lib.mjs";
export default async function (b) {
  const q = js => b.eval(js);
  await b.viewport(1440, 900);
  await b.allowClipboard(new URL(base).origin);
  await b.goto(base + `#devices/push/${process.env.PUSH_DEVICE}`, 2500);
  await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 5", 5000);
  const want = await q("document.querySelectorAll('.urls-panel .url-value')[2].textContent");
  await q("document.querySelectorAll('.urls-panel .copy-btn')[2].click()");
  await b.sleep(300);
  const got = await q("navigator.clipboard.readText()");
  check("真实剪贴板内容等于 HTTP-FLV 地址", got === want, got.slice(0, 60));
  check("复制按钮反馈", (await q("document.querySelectorAll('.urls-panel .copy-btn')[2].textContent")).includes("已复制"));
  summary();
}
