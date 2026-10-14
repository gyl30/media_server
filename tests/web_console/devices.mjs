// 设备管理（真实接口）：三类资源合并、五种接入协议创建与持久化、旧推流设备显示"未指定"、搜索筛选、
// RTSP 编辑与密码语义、推流添加时显示真实一次性地址（确认后生效、取消不创建）、删除与失败提示。RTSP 密码与推流协议通过 SQLite 核对（WEB_CONSOLE_DB）。
import { execFileSync } from "node:child_process";
import { base, shots, check, clickText, text, setInput, toasts, api, waitFor, summary } from "./lib.mjs";

const db = process.env.WEB_CONSOLE_DB;
const sql = query => execFileSync("sqlite3", [db, query]).toString().trim();
const LEGACY = process.env.LEGACY_PUSH_DEVICE;

export default async function (b) {
  const q = js => b.eval(js);
  const rowNames = () => q("[...document.querySelectorAll('.table tbody tr')].map(r => r.querySelector('.c-name a').textContent)");
  const row = name => `[...document.querySelectorAll('.table tbody tr')].find(r => r.querySelector('.c-name a').textContent === ${JSON.stringify(name)})`;
  const rowText = async name => q(`(${row(name)})?.innerText || ''`);
  const rowAction = (name, action) => q(`(${row(name)}).querySelector('[data-action="${action}"]').click()`);
  const openAdd = async () => { await q(clickText(".page-head .btn", "添加设备")); await b.sleep(200); };
  const submit = async (wait = 700) => { await q("document.querySelector('#device-dialog [type=submit]').click()"); await b.sleep(wait); };
  const dialogOpen = () => q("document.getElementById('device-dialog').open");
  const pick = value => q(`(() => { const s = document.querySelector('#device-dialog select[name=protocol]'); s.value = ${JSON.stringify(value)}; s.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  const confirm = async () => { await b.sleep(150); await q("document.querySelector('#confirm-dialog [data-confirm]').click()"); };

  // 清理上次中断留下的测试数据
  for (const device of (await api("/api/push-devices")).body.devices.filter(item => item.name.startsWith("测试"))) await api(`/api/push-devices/${device.device_id}`, { method: "DELETE" });
  for (const source of (await api("/api/sources")).body.sources.filter(item => item.stream_name.startsWith("测试"))) await api(`/api/sources/${source.source_id}`, { method: "DELETE" });
  await api("/api/devices/34020000001320000999", { method: "DELETE" });

  await b.viewport(1440, 900);
  await b.goto(base + "#devices", 2200);
  const counts = { gb: (await api("/api/devices")).body.devices.length, rtsp: (await api("/api/sources")).body.sources.length, push: (await api("/api/push-devices")).body.devices.length };
  check("列表合并三类接口", (await rowNames()).length === counts.gb + counts.rtsp + counts.push, JSON.stringify(counts));
  check("列表按名称排序", JSON.stringify(await rowNames()) === JSON.stringify([...(await rowNames())].sort((a, b) => a.localeCompare(b, "zh-CN"))));
  if (LEGACY) {
    const legacy = (await api(`/api/push-devices/${LEGACY}`)).body;
    check("旧推流设备升级后可读取且协议为空", legacy?.protocol === "", JSON.stringify(legacy));
    check("旧推流设备显示协议未指定", (await rowText(legacy.name)).includes("未指定"));
  }
  check("GB 设备行：协议、接入方式与编号", (await rowText("园区东门 NVR")).includes("GB28181") && (await rowText("园区东门 NVR")).includes("SIP 注册") && (await rowText("园区东门 NVR")).includes("34020000001320000101"));
  check("GB 设备有通道数量", await waitFor(b, `(${row("园区东门 NVR")}).querySelector('.num').textContent === '1'`, 5000));

  // 添加 GB：本地校验、服务端重复、成功
  await q("document.querySelector('.page-head .btn').focus()");
  await openAdd();
  check("对话框打开并聚焦接入协议", await dialogOpen() && await q("document.activeElement.name") === "protocol");
  check("默认 GB28181 显示国标编号字段", await q("!!document.querySelector('#device-dialog [name=device_id]') && !document.querySelector('#device-dialog [name=url]')"));
  await q(setInput("#device-dialog [name=name]", "测试 GB 设备"));
  await q(setInput("#device-dialog [name=device_id]", "123"));
  await submit(100);
  check("国标编号本地校验并聚焦", (await q(text("#device-dialog"))).includes("20 位数字") && await q("document.activeElement.name") === "device_id");
  await q(setInput("#device-dialog [name=device_id]", "34020000001320000101"));
  await submit();
  check("服务端重复编号：保留对话框与输入", await dialogOpen() && (await q(text("#device-dialog [role=alert]"))).includes("设备已存在") && await q("document.querySelector('#device-dialog [name=device_id]').value") === "34020000001320000101");
  await q(setInput("#device-dialog [name=device_id]", "34020000001320000999"));
  await submit();
  check("添加 GB 成功", !(await dialogOpen()) && (await q(toasts)).includes("等待设备注册"));
  check("焦点回到添加按钮", await q("document.activeElement.textContent.includes('添加设备')"));
  check("新 GB 设备显示离线", await waitFor(b, `(${row("测试 GB 设备")})?.innerText.includes('离线')`, 5000));

  // RTSP 拉流：字段随协议变化、URL 校验、创建
  await openAdd();
  await pick("rtsp-pull");
  check("RTSP 拉流显示地址与认证字段", await q("!!document.querySelector('#device-dialog [name=url]') && !!document.querySelector('#device-dialog [name=password]') && !document.querySelector('#device-dialog [name=device_id]')"));
  await q(setInput("#device-dialog [name=name]", "测试 RTSP 源"));
  await q(setInput("#device-dialog [name=url]", "rtsp://admin:x@10.0.0.1/live"));
  await submit(100);
  check("RTSP 地址不允许内嵌账号", (await q(text("#device-dialog"))).includes("不含账号密码"));
  await q(setInput("#device-dialog [name=url]", "rtsp://127.0.0.1:2/live"));
  await q(setInput("#device-dialog [name=username]", "admin"));
  await q(setInput("#device-dialog [name=password]", "first-pass"));
  await submit();
  const source = (await api("/api/sources")).body.sources.find(item => item.stream_name === "测试 RTSP 源");
  check("添加 RTSP 源并保存凭据", Boolean(source) && sql(`SELECT username || '|' || password FROM rtsp_sources WHERE source_id = '${source.source_id}'`) === "admin|first-pass");

  // 三种推流协议
  for (const [value, label] of [["rtmp", "RTMP"], ["rtsp", "RTSP"], ["whip", "WHIP"]]) {
    await openAdd();
    await pick(value);
    check(`${label} 推流只要求名称`, await q("!document.querySelector('#device-dialog [name=url]') && !document.querySelector('#device-dialog [name=device_id]') && !document.querySelector('#device-dialog [name=password]')"));
    check(`${label} 推流添加前显示一个所选协议地址`, await waitFor(b, "document.querySelectorAll('#device-dialog .url-value').length === 1", 3000));
    const shown = await q("document.querySelector('#device-dialog .url-value').textContent");
    const token = shown.split("/").pop();
    check(`${label} 地址形态`, shown.startsWith({ rtmp: "rtmp://127.0.0.1:21935/live/", rtsp: "rtsp://127.0.0.1:28554/", whip: "http://127.0.0.1:28080/publish/whip/" }[value]), shown);
    check(`${label} 说明确认后生效、5 分钟`, (await q(text("#device-dialog"))).includes("确定添加」后地址才生效，有效期 5 分钟"));
    if (value === "rtmp") {
      const verify = await api("/internal/verify", { method: "POST", body: { token, operation: "publish", stream_id: token } });
      check("确认前地址不能用于发布", verify.status === 403, String(verify.status));
      await b.shot(`${shots}/devices-add-push.png`);
      await q("document.querySelector('#device-dialog .btn:not(.primary)').click()");
      check("取消添加不创建设备", !(await api("/api/push-devices")).body.devices.some(item => item.token === token) && sql(`SELECT COUNT(*) FROM push_devices WHERE credential = '${token}'`) === "0");
      await openAdd();
      await pick(value);
      await waitFor(b, "document.querySelectorAll('#device-dialog .url-value').length === 1", 3000);
      check("重新打开生成新地址", (await q("document.querySelector('#device-dialog .url-value').textContent")) !== shown);
    }
    const confirmed = await q("document.querySelector('#device-dialog .url-value').textContent");
    await q(setInput("#device-dialog [name=name]", `测试 ${label} 推流`));
    await submit();
    const created = (await api("/api/push-devices")).body.devices.find(item => item.name === `测试 ${label} 推流`);
    check(`${label} 推流设备协议已持久化`, created?.protocol === value && sql(`SELECT protocol FROM push_devices WHERE device_id = '${created.device_id}'`) === value);
    check(`${label} 确认后地址与添加页一致且待接入`, created?.state === "pending" && created[`${value}_url`] === confirmed
      && Math.abs(Date.parse(created.expires_at) - Date.now() - 300000) < 5000, JSON.stringify(created));
  }
  await waitFor(b, `(${row("测试 WHIP 推流")})`, 5000);
  check("推流设备显示所选协议", (await rowText("测试 RTMP 推流")).includes("RTMP") && (await rowText("测试 WHIP 推流")).includes("WHIP") && (await rowText("测试 RTSP 推流")).includes("推流"));
  await b.goto(base + "#devices", 300);
  await b.reload(2200);
  check("重新加载页面协议不丢失", (await rowText("测试 WHIP 推流")).includes("WHIP") && (await rowText("测试 RTMP 推流")).includes("RTMP"));

  // 搜索与筛选
  const filter = (select, value) => q(`(() => { const s = document.querySelectorAll('.toolbar select')[${select}]; s.value = ${JSON.stringify(value)}; s.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  await q(setInput(".toolbar input[type=search]", "测试"));
  check("搜索名称", (await rowNames()).length === 5, JSON.stringify(await rowNames()));
  await q(setInput(".toolbar input[type=search]", "whip"));
  check("搜索协议", (await rowNames()).includes("测试 WHIP 推流") && !(await rowNames()).includes("测试 RTMP 推流"));
  await q(setInput(".toolbar input[type=search]", source.source_id.slice(0, 8)));
  check("搜索资源标识", JSON.stringify(await rowNames()) === JSON.stringify(["测试 RTSP 源"]));
  await q(setInput(".toolbar input[type=search]", ""));
  await filter(0, "rtmp-push");
  check("协议筛选：RTMP 推流", (await rowNames()).includes("测试 RTMP 推流") && (await rowNames()).every(name => !name.includes("WHIP")));
  await filter(0, "push-unspecified");
  check("协议筛选：推流未指定", LEGACY ? (await rowNames()).length >= 1 && !(await rowNames()).includes("测试 RTMP 推流") : true);
  await filter(0, "all");
  await filter(1, "online");
  check("状态筛选：在线", (await rowNames()).includes("园区东门 NVR") && !(await rowNames()).includes("测试 GB 设备"));
  await filter(1, "all");
  await q(setInput(".toolbar input[type=search]", "不存在的设备"));
  check("无匹配空状态", (await q(text(".card"))).includes("没有符合条件的设备"));
  await q(clickText(".card .btn", "清除筛选"));

  // 按类型的操作
  check("GB 操作：详情 / 通道 / 删除", (await q(`(${row("园区东门 NVR")}).querySelector('.text-actions').innerText`)).replace(/\s+/g, " ").trim() === "详情 通道 删除");
  check("RTSP 操作含编辑与开始拉流", (await q(`(${row("测试 RTSP 源")}).querySelector('.text-actions').innerText`)).includes("开始拉流"));
  check("推流操作：详情 / 编辑 / 删除（未推流不显示预览，不再有签发）", (await q(`(${row("测试 RTMP 推流")}).querySelector('.text-actions').innerText`)).replace(/\s+/g, " ").trim() === "详情 编辑 删除");
  check("推流行状态为待接入并显示地址", (await rowText("测试 RTMP 推流")).includes("待接入") && (await rowText("测试 RTMP 推流")).includes("rtmp://"));

  // 编辑 RTSP：URL / 新密码 / 留空保留 / 清除
  await rowAction("测试 RTSP 源", "edit"); await b.sleep(200);
  check("编辑表单不回显密码", await q("document.querySelector('#device-dialog [name=password]').value") === "");
  await q(setInput("#device-dialog [name=url]", "rtsp://127.0.0.1:3/main"));
  await q(setInput("#device-dialog [name=password]", "second-pass"));
  await submit();
  check("保存 URL 与新密码", sql(`SELECT url || '|' || password FROM rtsp_sources WHERE source_id = '${source.source_id}'`) === "rtsp://127.0.0.1:3/main|second-pass" && (await q(toasts)).includes("更新了密码"));
  await rowAction("测试 RTSP 源", "edit"); await b.sleep(200);
  await q(setInput("#device-dialog [name=name]", "测试 RTSP 源 改"));
  await submit();
  check("密码留空保留原密码", sql(`SELECT password FROM rtsp_sources WHERE source_id = '${source.source_id}'`) === "second-pass");
  await waitFor(b, `(${row("测试 RTSP 源 改")})`, 5000);
  await rowAction("测试 RTSP 源 改", "edit"); await b.sleep(200);
  await q(clickText("#device-dialog label.check", "清除已保存的密码"));
  await submit();
  check("清除密码保留用户名", sql(`SELECT username || '|' || password FROM rtsp_sources WHERE source_id = '${source.source_id}'`) === "admin|" && (await q(toasts)).includes("清除了已保存的密码"));
  check("界面不出现密码", !(await q("document.body.innerText")).includes("second-pass"));

  // 编辑推流设备：名称与协议
  await rowAction("测试 RTMP 推流", "edit"); await b.sleep(200);
  check("推流编辑显示当前协议", await q("document.querySelector('#device-dialog select[name=protocol]').value") === "rtmp");
  await pick("whip");
  await q(setInput("#device-dialog [name=name]", "测试 RTMP 推流 改"));
  await submit();
  const edited = (await api("/api/push-devices")).body.devices.find(item => item.name === "测试 RTMP 推流 改");
  check("编辑推流设备名称与协议", edited?.protocol === "whip");
  if (LEGACY) {
    const legacyName = (await api(`/api/push-devices/${LEGACY}`)).body.name;
    await rowAction(legacyName, "edit"); await b.sleep(200);
    check("旧设备编辑保留未指定选项", await q("document.querySelector('#device-dialog select[name=protocol]').value") === "");
    await q("document.getElementById('device-dialog').close()");
  }

  if (LEGACY) {
    const legacy = (await api(`/api/push-devices/${LEGACY}`)).body;
    check("旧推流设备没有地址时显示未生成地址", legacy.state !== "none" || (await rowText(legacy.name)).includes("未生成地址"));
  }
  await b.shot(`${shots}/devices-1440.png`);

  // 删除与失败提示
  await rowAction("测试 GB 设备", "delete"); await b.sleep(200);
  check("删除确认默认聚焦取消", await q("document.activeElement.textContent") === "取消");
  await q("window.__fail = true; const original = window.fetch; window.fetch = (url, options) => window.__fail && options?.method === 'DELETE' ? Promise.resolve(new Response(JSON.stringify({ error: 'device_store_failed' }), { status: 500, headers: { 'Content-Type': 'application/json' } })) : original(url, options)");
  await q("document.querySelector('#confirm-dialog [data-confirm]').click()"); await b.sleep(400);
  check("删除失败保留确认框并显示原因", await q("document.getElementById('confirm-dialog').open") && (await q(text("#confirm-dialog"))).includes("暂时无法保存"));
  await q("window.__fail = false");
  await q("document.querySelector('#confirm-dialog [data-confirm]').click()");
  for (const name of ["测试 GB 设备", "测试 RTSP 源 改", "测试 RTMP 推流 改", "测试 RTSP 推流", "测试 WHIP 推流"]) {
    await waitFor(b, `!(${row(name)}) || !document.getElementById('confirm-dialog').open`, 5000);
    if (await q(`!!(${row(name)})`)) {
      await rowAction(name, "delete");
      await confirm();
    }
    check(`删除 ${name}`, await waitFor(b, `!(${row(name)})`, 6000));
  }
  summary();
}
