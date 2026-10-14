// Firefox（WebDriver BiDi）冒烟：逐页加载、检查横向溢出与控制台错误、截图，并在视频中心实际播放一路 WHEP。
// 用法：node tests/web_console/bidi.mjs（需要 firefox 与正在运行的测试服务）。
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { base, shots, check, summary } from "./lib.mjs";

const port = Number(process.env.WEB_CONSOLE_BIDI_PORT || 9444);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("invalid WEB_CONSOLE_BIDI_PORT");
const profile = mkdtempSync(join(process.env.HOME, ".cache", "msc-firefox-"));
const firefox = spawn("firefox", ["--headless", "--remote-debugging-port", String(port), "--profile", profile, "--no-remote"], { stdio: "ignore" });
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let socket;
let startError;
firefox.on("error", error => { startError = error; });
try {
for (let attempt = 0; attempt < 60 && !socket; attempt++) {
  if (startError) throw startError;
  if (firefox.exitCode !== null || firefox.signalCode !== null) throw new Error("owned Firefox exited during startup");
  await sleep(500);
  try {
    const candidate = new WebSocket(`ws://127.0.0.1:${port}/session`);
    await new Promise((resolve, reject) => { candidate.onopen = resolve; candidate.onerror = reject; });
    socket = candidate;
  } catch { /* 等待浏览器启动 */ }
}
if (!socket) throw new Error("owned Firefox BiDi startup timed out");
let id = 0;
const pending = new Map();
const errors = [];
socket.onmessage = event => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    const { resolve, reject } = pending.get(message.id);
    pending.delete(message.id);
    message.type === "error" ? reject(new Error(`${message.error}: ${message.message}`)) : resolve(message.result);
  } else if (message.method === "log.entryAdded" && message.params.level === "error") {
    errors.push(message.params.text);
  }
};
const send = (method, params = {}) => new Promise((resolve, reject) => {
  const current = ++id;
  pending.set(current, { resolve, reject });
  socket.send(JSON.stringify({ id: current, method, params }));
});
  await send("session.new", { capabilities: {} });
  await send("session.subscribe", { events: ["log.entryAdded"] });
  const context = (await send("browsingContext.getTree", {})).contexts[0].context;
  await send("browsingContext.setViewport", { context, viewport: { width: 1440, height: 900 } });
  const evaluate = async expression => {
    const response = await send("script.evaluate", { expression, target: { context }, awaitPromise: true });
    if (response.type === "exception") throw new Error(`evaluate: ${response.exceptionDetails.text}`);
    return response.result.value;
  };
  for (const route of ["overview", "devices", "devices/gb/34020000001320000101", "monitor", "resources", "status"]) {
    await send("browsingContext.navigate", { context, url: `${base}#${route}`, wait: "complete" });
    await sleep(2500);
    const overflow = await evaluate("document.documentElement.scrollWidth - innerWidth");
    check(`Firefox ${route} 加载且无横向溢出`, overflow <= 0 && (await evaluate("document.querySelector('#view').innerText.length")) > 20, String(overflow));
    const { data } = await send("browsingContext.captureScreenshot", { context });
    writeFileSync(`${shots}/firefox-${route.split("/")[0]}.png`, Buffer.from(data, "base64"));
  }
  await send("browsingContext.navigate", { context, url: `${base}#monitor`, wait: "complete" });
  await sleep(2500);
  console.log("Firefox native video codecs:", await evaluate("JSON.stringify(RTCRtpReceiver.getCapabilities('video').codecs.map(codec => codec.mimeType))"));
  await evaluate("[...document.querySelectorAll('.monitor-play')].find(button => button.dataset.key.startsWith('play:push:')).click()");
  await sleep(8000);
  const state = await evaluate("(() => { const v = document.querySelector('.tile video'); return document.querySelector('.grid-wrap .tile').className + ' t=' + (v ? v.currentTime.toFixed(1) : '-') + ' w=' + (v ? v.videoWidth : 0) + ' ' + document.querySelector('.grid-wrap .tile').innerText.replace(/\\n/g, ' '); })()");
  check("Firefox 实时监控 WHEP 播放", /is-streaming/.test(state) && /w=1280/.test(state), state);
  const { data } = await send("browsingContext.captureScreenshot", { context });
  writeFileSync(`${shots}/firefox-video-playing.png`, Buffer.from(data, "base64"));
  await send("browsingContext.navigate", { context, url: `${base}#overview`, wait: "complete" });
  await sleep(1000);
  check("Firefox 控制台无错误", errors.length === 0, JSON.stringify(errors.slice(0, 3)));
} catch (error) {
  console.error("SCRIPT FAILED:", error.message);
  process.exitCode = 1;
} finally {
  summary();
  socket?.close();
  if (firefox.pid && firefox.exitCode === null && firefox.signalCode === null) {
    firefox.kill("SIGTERM");
    for (let attempt = 0; attempt < 50 && firefox.exitCode === null && firefox.signalCode === null; attempt++) await sleep(100);
    if (firefox.exitCode === null && firefox.signalCode === null) {
      firefox.kill("SIGKILL");
      await new Promise(resolve => firefox.once("exit", resolve));
    }
  }
  if (process.exitCode) console.log("FAILED_PROFILE:", profile);
  else rmSync(profile, { recursive: true, force: true });
}
