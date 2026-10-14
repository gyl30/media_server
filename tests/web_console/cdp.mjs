// Minimal headless-Chrome driver over CDP (Node 22 global WebSocket). 用法：WEB_CONSOLE_URL=http://127.0.0.1:29090/ node tests/web_console/cdp.mjs tests/web_console/<脚本>.mjs
import { spawn } from "node:child_process";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const port = 9333;
const profile = mkdtempSync(join(tmpdir(), "msc-chrome-"));
const chrome = spawn("google-chrome", ["--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
  "--no-first-run", "--no-default-browser-check", "--disable-gpu", "--hide-scrollbars", "about:blank"], { stdio: "ignore" });

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let version;
for (let attempt = 0; attempt < 50 && !version; attempt++) {
  try { version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(200); }
}
const browser = new WebSocket(version.webSocketDebuggerUrl);
await new Promise(resolve => browser.addEventListener("open", resolve, { once: true }));
let nextID = 0;
const pending = new Map();
const logs = [];
browser.addEventListener("message", event => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    const { resolve, reject } = pending.get(message.id);
    pending.delete(message.id);
    message.error ? reject(new Error(JSON.stringify(message.error))) : resolve(message.result);
    return;
  }
  if (message.method === "Runtime.consoleAPICalled") {
    logs.push(`[console.${message.params.type}] ${message.params.args.map(arg => arg.value ?? arg.description ?? "").join(" ")}`);
  } else if (message.method === "Runtime.exceptionThrown") {
    const details = message.params.exceptionDetails;
    logs.push(`[exception] ${details.exception?.description || details.text}`);
  } else if (message.method === "Log.entryAdded") {
    logs.push(`[log.${message.params.entry.level}] ${message.params.entry.text} ${message.params.entry.url || ""}`);
  }
});
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = ++nextID;
  pending.set(id, { resolve, reject });
  browser.send(JSON.stringify({ id, method, params, sessionId }));
});
const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
const page = (method, params) => send(method, params, sessionId);
await page("Runtime.enable");
await page("Log.enable");
await page("Page.enable");

const api = {
  logs, sleep,
  async viewport(width, height) {
    await page("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: width < 600 });
  },
  async goto(url, wait = 2500) {
    await page("Page.navigate", { url });
    await sleep(wait);
  },
  async eval(expression) {
    const result = await page("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
    return result.result.value;
  },
  async click(x, y) {
    for (const type of ["mouseMoved", "mousePressed", "mouseReleased"]) {
      await page("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1 });
    }
  },
  async shot(path, full = false) {
    const params = { format: "png" };
    if (full) {
      const metrics = await page("Page.getLayoutMetrics");
      params.captureBeyondViewport = true;
      params.clip = { x: 0, y: 0, width: metrics.cssContentSize.width, height: metrics.cssContentSize.height, scale: 1 };
    }
    const { data } = await page("Page.captureScreenshot", params);
    writeFileSync(path, Buffer.from(data, "base64"));
  },
};

try {
  const script = await import(pathToFileURL(resolve(process.argv[2])).href);
  await script.default(api);
} catch (error) {
  console.error("SCRIPT FAILED:", error.message);
  process.exitCode = 1;
} finally {
  console.log("---- browser logs ----");
  for (const line of logs) console.log(line);
  browser.close();
  chrome.kill();
}
