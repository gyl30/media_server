// 正式 Web 控制台浏览器测试的公共函数。被测服务由 WEB_CONSOLE_URL 指定（signaling HTTP 地址）。
export const base = (process.env.WEB_CONSOLE_URL || "http://127.0.0.1:29090/").replace(/\/?$/, "/");
export const shots = process.env.WEB_CONSOLE_SHOTS || new URL("./shots/", import.meta.url).pathname;
export const results = [];
export const check = (label, cond, extra = "") => { results.push([Boolean(cond), label]); console.log(cond ? "PASS" : "FAIL", label, extra); };
export const clickText = (selector, text, nth = 0) => `(() => { const el = [...document.querySelectorAll(${JSON.stringify(selector)})].filter(e => e.textContent.includes(${JSON.stringify(text)}))[${nth}]; if (!el) throw new Error("missing " + ${JSON.stringify(selector + " " + text)}); el.click(); return true; })()`;
export const text = sel => `(document.querySelector(${JSON.stringify(sel)})?.innerText || "")`;
export const setInput = (sel, value) => `(() => { const el = document.querySelector(${JSON.stringify(sel)}); const proto = el instanceof HTMLSelectElement ? HTMLSelectElement.prototype : HTMLInputElement.prototype; Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, ${JSON.stringify(value)}); el.dispatchEvent(new Event(el instanceof HTMLSelectElement ? 'change' : 'input', { bubbles: true })); return true; })()`;
export const toasts = text("#toasts");
export const api = async (path, options = {}) => {
  const response = await fetch(new URL(path, base), { method: options.method || "GET", headers: { "Content-Type": "application/json" }, body: options.body ? JSON.stringify(options.body) : undefined });
  const body = response.status === 204 ? null : await response.json().catch(() => null);
  return { status: response.status, body };
};
export async function waitFor(b, js, timeout = 10000, interval = 200) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await b.eval(js)) return true;
    await b.sleep(interval);
  }
  return false;
}
export function summary() {
  const failed = results.filter(([ok]) => !ok);
  console.log(`== ${results.length - failed.length}/${results.length} PASS`);
  for (const [, label] of failed) console.log("FAILED:", label);
  if (failed.length) process.exitCode = 1;
}
