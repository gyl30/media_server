// 设备详情的管理操作（真实接口）：GB 点播 / 停止、RTSP 自动拉流失败后重试与停止、推流接入地址查看 / 复制 / 重新生成。
import { base, shots, check, clickText, text, toasts, api, waitFor, summary } from "./lib.mjs";
import { execFileSync } from "node:child_process";

const sql = query => execFileSync("sqlite3", [process.env.WEB_CONSOLE_DB, query]).toString().trim();

const GB = "34020000001320000101";
const CHANNEL = "34020000001310000001";

export default async function (b) {
  const q = js => b.eval(js);
  const liveOf = async () => (await api(`/api/devices/${GB}/channels`)).body.channels.find(item => item.channel_id === CHANNEL)?.live;
  const confirm = async () => { await b.sleep(150); await q("document.querySelector('#confirm-dialog [data-confirm]').click()"); };
  await b.viewport(1440, 900);
  // 清理上次中断留下的测试数据（名称唯一）。
  for (const source of (await api("/api/sources")).body.sources.filter(item => item.stream_name === "详情测试 RTSP")) {
    await api(`/api/sources/${source.source_id}`, { method: "DELETE" });
  }
  for (const device of (await api("/api/push-devices")).body.devices.filter(item => item.name === "详情测试 推流")) {
    await api(`/api/push-devices/${device.device_id}`, { method: "DELETE" });
  }

  // GB：打开详情不点播；行内点播；停止点播
  check("初始无点播", !(await liveOf()));
  await b.goto(base + `#devices/gb/${GB}`, 2000);
  await b.sleep(3000);
  check("打开详情后仍未点播", !(await liveOf()) && (await q(text(".channel-list"))).includes("未点播"));
  check("点播请求期间按钮忙碌", await q(`(() => { document.querySelector('[data-key="live:${CHANNEL}"]').click(); return document.querySelector('[data-key="live:${CHANNEL}"]')?.disabled === true; })()`));
  check("点播完成后状态为点播中", await waitFor(b, "document.querySelector('.channel-list').innerText.includes('点播中')", 15000));
  check("后端 live 为 streaming", (await liveOf())?.state === "streaming");
  await b.shot(`${shots}/detail-gb-live.png`);
  await q(`document.querySelector('[data-key="stop:${CHANNEL}"]').click()`);
  await b.sleep(200);
  check("停止点播需确认且默认聚焦取消", await q("document.getElementById('confirm-dialog').open && document.activeElement.textContent === '取消'"));
  await confirm();
  check("停止点播后后端无 live", await waitFor(b, "fetch('/api/devices/" + GB + "/channels').then(r => r.json()).then(j => !j.channels[0].live)", 10000));
  check("界面回到未点播", await waitFor(b, "document.querySelector('.channel-list').innerText.includes('未点播')", 6000));

  // 管理点播期间离开：上游请求可以成功，迟到的 finally/update 不能重新挂回旧详情。
  await q(`window.__delayLive = true; const priorFetch = window.fetch; window.fetch = async (url, options) => {
    if (window.__delayLive && String(url).endsWith('/channels/${CHANNEL}/play')) await new Promise(resolve => setTimeout(resolve, 1500));
    return priorFetch(url, options);
  }`);
  await q(`document.querySelector('[data-key="live:${CHANNEL}"]').click()`);
  await q("location.hash = '#devices'");
  await b.sleep(3000);
  check("点播迟到完成不覆盖新路由页面", await q("location.hash === '#devices' && document.querySelector('.page h1').textContent === '设备管理' && !document.querySelector('.detail-player')"));
  check("离页只撤销详情所有权，不撤销已提交上游点播", (await liveOf())?.state === "streaming");
  await q("window.__delayLive = false");
  const lateLive = await liveOf();
  if (lateLive) await api(`/api/lives/${lateLive.live_id}`, { method: "DELETE" });

  // RTSP：添加即自动拉流；地址不可达时会话结束，配置保留，显示连接失败并可重试
  const created = await api("/api/sources", { method: "POST", body: { stream_name: "详情测试 RTSP", url: "rtsp://127.0.0.1:9/none" } });
  const sourceID = created.body.source_id;
  check("创建即自动拉流", created.body.desired_state === "running" && Boolean(created.body.session), JSON.stringify(created.body));
  await b.goto(base + `#devices/rtsp/${sourceID}`, 1500);
  check("接口：拉流结束后无会话且配置保留", await waitFor(b, `fetch('/api/sources').then(r => r.json()).then(j => j.sources.some(s => s.source_id === '${sourceID}' && s.desired_state === 'running' && !s.session))`, 15000));
  check("界面显示连接失败并提供重试连接，不提供开始拉流", await waitFor(b, "document.querySelector('.detail-head').innerText.includes('连接失败或已结束')", 6000)
    && (await q(text(".detail-head"))).includes("重试连接") && !(await q(text(".page"))).includes("开始拉流"));
  await q(clickText(".detail-head .btn", "重试连接"));
  check("重试后重新发起会话", await waitFor(b, `document.querySelector('#toasts')?.innerText.includes('已重新发起连接')`, 5000));
  check("会话再次结束后显示连接失败", await waitFor(b, `fetch('/api/sources').then(r => r.json()).then(j => j.sources.some(s => s.source_id === '${sourceID}' && !s.session))
    .then(ended => ended && document.querySelector('.page').innerText.includes('连接失败或已结束'))`, 15000));
  await b.shot(`${shots}/detail-rtsp-unconnected.png`);
  await api(`/api/sources/${sourceID}`, { method: "DELETE" });

  // 推流：详情显示同一地址与状态；待接入时不提供重新生成；过期后重新生成，旧地址失效
  const push = (await api("/api/push-devices", { method: "POST", body: { name: "详情测试 推流" } })).body;
  await b.goto(base + `#devices/push/${push.device_id}`, 1500);
  const urls = await q("[...document.querySelectorAll('.page .url-value')].map(e => e.textContent)");
  check("未指定协议显示三种地址且与接口一致", urls.length === 3 && urls[0] === push.rtmp_url && urls[1] === push.rtsp_url && urls[2] === push.whip_url, urls.join(" "));
  check("显示待接入与截止时间", (await q(text(".page"))).includes("待接入") && (await q(text(".page"))).includes("前首次接入有效"));
  check("待接入时不提供重新生成", !(await q("!!document.querySelector('[data-key=push-renew]')")));
  check("不单独显示 token 字段", !(await q(text(".page"))).toLowerCase().includes("token"));
  await b.shot(`${shots}/detail-push-pending.png`);
  sql(`UPDATE push_devices SET credential_expires_at = 1 WHERE device_id = '${push.device_id}'`);
  await q(clickText(".topbar .btn", "刷新"));
  check("过期后显示地址已过期", await waitFor(b, "document.querySelector('.page').innerText.includes('地址已过期')", 5000));
  await q("document.querySelector('[data-key=push-renew]').click()");
  check("重新生成后回到待接入并显示新地址", await waitFor(b, `document.querySelector('.page .url-value') && document.querySelector('.page .url-value').textContent !== ${JSON.stringify(urls[0])} && document.querySelector('.page').innerText.includes('待接入')`, 5000));
  const verify = await api("/internal/verify", { method: "POST", body: { token: push.token, operation: "publish", stream_id: push.token } });
  check("旧地址已失效", verify.status === 403, String(verify.status));
  await api(`/api/push-devices/${push.device_id}`, { method: "DELETE" });
  summary();
}
