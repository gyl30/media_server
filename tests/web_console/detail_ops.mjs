// 设备详情的管理操作（真实接口）：GB 点播 / 停止、RTSP 开始 / 停止取流、推流接入地址查看 / 复制 / 重新生成。
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

  // RTSP：开始取流（地址不可达）与停止
  const created = await api("/api/sources", { method: "POST", body: { stream_name: "详情测试 RTSP", url: "rtsp://127.0.0.1:9/none" } });
  const sourceID = created.body.source_id;
  await b.goto(base + `#devices/rtsp/${sourceID}`, 1500);
  check("RTSP 详情显示未启动", (await q(text(".detail-head"))).includes("未启动"), await q("location.hash + ' ' + document.querySelector('#view').innerText.slice(0, 120)"));
  await q(clickText(".detail-head .btn", "开始拉流"));
  // 不可达地址：服务端短暂建立会话后结束，期望状态保持 running、没有会话。
  check("接口：拉流结束后无会话", await waitFor(b, `fetch('/api/sources').then(r => r.json()).then(j => j.sources.some(s => s.source_id === '${sourceID}' && s.desired_state === 'running' && !s.session))`, 15000));
  check("界面显示未连接并提供重新取流与停止", await waitFor(b, "document.querySelector('.detail-head').innerText.includes('未连接')", 6000)
    && (await q(text(".detail-head"))).includes("重新拉流") && (await q(text(".detail-head"))).includes("停止拉流"));
  await b.shot(`${shots}/detail-rtsp-unconnected.png`);
  await q(clickText(".detail-head .btn", "停止拉流"));
  await confirm();
  await b.sleep(1500);
  check("停止后期望状态为 stopped", (await api("/api/sources")).body.sources.find(item => item.source_id === sourceID).desired_state === "stopped");
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
