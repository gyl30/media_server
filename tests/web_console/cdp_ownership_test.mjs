// 仅使用自有假 Chrome/HTTP 哨兵；占用端口不得被当成本轮浏览器。
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = mkdtempSync(join(tmpdir(), "msc-cdp-ownership-"));
const script = join(root, "empty.mjs");
const harness = resolve(process.argv[2] || fileURLToPath(new URL("cdp.mjs", import.meta.url)));
let requests = 0, upgrades = 0;
const sentinel = createServer((request, response) => {
  requests++;
  response.setHeader("Content-Type", "application/json");
  response.end(JSON.stringify({ webSocketDebuggerUrl: `ws://127.0.0.1:${sentinel.address().port}/devtools/browser/sentinel` }));
});
sentinel.on("upgrade", (request, socket) => { upgrades++; socket.destroy(); });
let child;
try {
  // 在新浏览器退出之前，旧驱动会查询哨兵并尝试 WebSocket；假进程永远不提供 CDP。
  writeFileSync(join(root, "google-chrome"), `#!${process.execPath}\nsetTimeout(() => process.exit(1), 500);\n`, { mode: 0o700 });
  writeFileSync(script, "export default async function () { throw new Error('fake Chrome cannot run a page'); }\n");
  await new Promise((resolve, reject) => { sentinel.once("error", reject); sentinel.listen(0, "127.0.0.1", resolve); });
  child = spawn(process.execPath, [harness, script], {
    env: { ...process.env, TMPDIR: root, PATH: `${root}:${process.env.PATH}`, WEB_CONSOLE_CDP_PORT: String(sentinel.address().port) },
    stdio: ["ignore", "pipe", "pipe"], timeout: 15000,
  });
  let output = "";
  child.stdout.on("data", data => { output += data; });
  child.stderr.on("data", data => { output += data; });
  const code = await new Promise((resolve, reject) => { child.once("error", reject); child.once("close", resolve); });
  console.log(JSON.stringify({ exit_status: code, sentinel_requests: requests, sentinel_upgrades: upgrades, output }));
  assert.equal(code, 1, "自有假浏览器失败必须使驱动失败");
  assert.equal(requests, 0, "驱动访问了不属于自有 Chrome 的 HTTP 端口");
  assert.equal(upgrades, 0, "驱动连接了不属于自有 Chrome 的 CDP");
  console.log("PASS: occupied CDP endpoint was never contacted");
} finally {
  if (child && child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
  sentinel.closeAllConnections();
  await new Promise(resolve => sentinel.close(resolve));
  rmSync(root, { recursive: true, force: true });
}
