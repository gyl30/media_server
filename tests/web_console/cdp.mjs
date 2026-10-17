// Minimal headless-Chrome driver over CDP (Node 22 global WebSocket). 用法：WEB_CONSOLE_URL=http://127.0.0.1:29090/ node tests/web_console/cdp.mjs tests/web_console/<脚本>.mjs
import { spawn } from "node:child_process";
import { writeFileSync, mkdtempSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const port = Number(process.env.WEB_CONSOLE_CDP_PORT || 9333);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("invalid WEB_CONSOLE_CDP_PORT");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let chrome;
let browser;
const logs = [];
const audioFixture = process.env.WEB_CONSOLE_AUDIO_FIXTURE;
const audioArgs = [];
if (audioFixture) {
  const file = resolve(audioFixture);
  if (!statSync(file).isFile()) throw new Error("audio fixture must be a regular WAV file");
  audioArgs.push("--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream", `--use-file-for-fake-audio-capture=${file}`);
}
const profile = mkdtempSync(join(tmpdir(), "msc-chrome-"));
try {
let startError;
chrome = spawn("google-chrome", ["--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
  "--no-first-run", "--no-default-browser-check", "--disable-gpu", "--hide-scrollbars", ...audioArgs, "about:blank"], { stdio: "ignore" });
chrome.on("error", error => { startError = error; });
let version;
for (let attempt = 0; attempt < 50 && !version; attempt++) {
  if (startError) throw startError;
  if (chrome.exitCode !== null || chrome.signalCode !== null) throw new Error("owned Chrome exited during startup");
  try { version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(200); }
}
if (!version?.webSocketDebuggerUrl) throw new Error("owned Chrome DevTools startup timed out");
browser = new WebSocket(version.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  browser.addEventListener("open", resolve, { once: true });
  browser.addEventListener("error", () => reject(new Error("Chrome DevTools connection failed")), { once: true });
});
let nextID = 0;
const pending = new Map();
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
  // 在每次文档加载、页面脚本执行之前注入；返回的 identifier 用于移除。
  async beforeLoad(source) {
    return (await page("Page.addScriptToEvaluateOnNewDocument", { source })).identifier;
  },
  async removeBeforeLoad(identifier) {
    await page("Page.removeScriptToEvaluateOnNewDocument", { identifier });
  },
  async hide() {
    const { targetId: background } = await send("Target.createTarget", { url: "about:blank" });
    await send("Target.activateTarget", { targetId: background });
    return background;
  },
  async show(background) {
    await send("Target.activateTarget", { targetId });
    await send("Target.closeTarget", { targetId: background });
  },
  async reload(wait = 2500) {
    await page("Page.reload", {});
    await sleep(wait);
  },
  // 真实剪贴板：授予权限并模拟页面获得焦点（无头模式默认不聚焦，读写剪贴板会被拒绝）。
  async allowClipboard(origin) {
    await send("Browser.grantPermissions", { origin, permissions: ["clipboardReadWrite", "clipboardSanitizedWrite"] });
    await page("Emulation.setFocusEmulationEnabled", { enabled: true });
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

  const script = await import(pathToFileURL(resolve(process.argv[2])).href);
  await script.default(api);
} catch (error) {
  console.error("SCRIPT FAILED:", error.message);
  process.exitCode = 1;
} finally {
  console.log("---- browser logs ----");
  for (const line of logs) console.log(line);
  browser?.close();
  // 只回收本 harness spawn 的 Chrome；启动失败也走这里，不按端口或名称杀进程。
  if (chrome?.pid && chrome.exitCode === null && chrome.signalCode === null) {
    chrome.kill("SIGTERM");
    for (let attempt = 0; attempt < 50 && chrome.exitCode === null && chrome.signalCode === null; attempt++) await sleep(100);
    if (chrome.exitCode === null && chrome.signalCode === null) {
      chrome.kill("SIGKILL");
      await new Promise(resolve => chrome.once("exit", resolve));
    }
  }
  if (process.exitCode) console.log("FAILED_PROFILE:", profile);
  else rmSync(profile, { recursive: true, force: true });
}
