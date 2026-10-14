// 设备管理：真实 API 的增删改、筛选、表单校验与失败保留。RTSP 密码通过 SQLite 数据库核对（WEB_CONSOLE_DB）。
import { execFileSync } from "node:child_process";
import { base, shots, check, clickText, text, setInput, toasts, api, waitFor, summary } from "./lib.mjs";

const db = process.env.WEB_CONSOLE_DB;
const storedPassword = sourceID => execFileSync("sqlite3", [db, `SELECT username || '|' || password FROM rtsp_sources WHERE source_id = '${sourceID}'`]).toString().trim();

export default async function (b) {
  const q = js => b.eval(js);
  const rows = () => q("[...document.querySelectorAll('.table tbody tr')].map(r => r.querySelector('.c-name').innerText.split('\\n')[0])");
  const openAdd = async () => { await q(clickText(".page-head .btn", "添加设备")); await b.sleep(200); };
  const submit = async (wait = 600) => { await q("document.querySelector('#device-dialog [type=submit]').click()"); await b.sleep(wait); };
  const dialogOpen = () => q("document.getElementById('device-dialog').open");

  await b.viewport(1440, 900);
  await b.goto(base + "#devices", 2000);
  const before = (await api("/api/devices")).body.devices.length + (await api("/api/sources")).body.sources.length + (await api("/api/push-devices")).body.devices.length;
  check("列表数量与三类接口之和一致", (await rows()).length === before, `${(await rows()).length} vs ${before}`);

  // 添加 GB：本地校验、服务端重复、成功
  await q("document.querySelector('.page-head .btn').focus()");
  await openAdd();
  check("对话框打开并聚焦名称", await dialogOpen() && await q("document.activeElement.name") === "name");
  await q(setInput("#device-dialog [name=name]", "测试 GB 设备"));
  await q(setInput("#device-dialog [name=device_id]", "123"));
  await submit(100);
  check("GB 编码本地校验并聚焦错误字段", (await q(text("#device-dialog"))).includes("20 位数字") && await q("document.activeElement.name") === "device_id");
  await q(setInput("#device-dialog [name=device_id]", "34020000001320000101"));
  await submit();
  check("服务端重复 ID：保留对话框与输入", await dialogOpen() && (await q(text("#device-dialog [role=alert]"))).includes("设备已存在")
    && await q("document.querySelector('#device-dialog [name=device_id]').value") === "34020000001320000101");
  await q(setInput("#device-dialog [name=device_id]", "34020000001320000999"));
  await submit();
  check("添加 GB 成功并关闭", !(await dialogOpen()) && (await q(toasts)).includes("已添加设备"));
  check("焦点回到添加按钮", await q("document.activeElement.textContent.includes('添加设备')"));
  await waitFor(b, "document.querySelector('.table tbody').innerText.includes('测试 GB 设备')");
  check("新 GB 设备显示为离线", (await q("[...document.querySelectorAll('.table tbody tr')].find(r => r.innerText.includes('测试 GB 设备'))?.innerText || ''")).includes("离线"));

  // 添加 RTSP 源与推流设备
  await openAdd();
  await q(clickText("#device-dialog .proto-option", "RTSP 拉流"));
  await q(setInput("#device-dialog [name=name]", "测试 RTSP 源"));
  await q(setInput("#device-dialog [name=url]", "rtsp://admin:x@10.0.0.1/live"));
  await submit(100);
  check("RTSP 地址不允许内嵌账号", (await q(text("#device-dialog"))).includes("不含账号密码"));
  await q(setInput("#device-dialog [name=url]", "rtsp://127.0.0.1:2/live"));
  await q(setInput("#device-dialog [name=username]", "admin"));
  await q(setInput("#device-dialog [name=password]", "first-pass"));
  await submit();
  const source = (await api("/api/sources")).body.sources.find(item => item.stream_name === "测试 RTSP 源");
  check("添加 RTSP 源", Boolean(source) && storedPassword(source.source_id) === "admin|first-pass");
  await openAdd();
  await q(clickText("#device-dialog .proto-option", "推流设备"));
  check("推流设备说明协议未固定", (await q(text("#device-dialog"))).includes("不支持为设备固定推流协议"));
  await q(setInput("#device-dialog [name=name]", "测试 推流设备"));
  await submit();
  const push = (await api("/api/push-devices")).body.devices.find(item => item.name === "测试 推流设备");
  check("添加推流设备", Boolean(push));
  await waitFor(b, "document.querySelector('.table tbody').innerText.includes('测试 推流设备')");

  // 搜索与筛选
  await q(setInput(".toolbar input[type=search]", "测试"));
  check("搜索名称", (await rows()).length === 3);
  await q(setInput(".toolbar input[type=search]", source.source_id.slice(0, 8)));
  check("搜索 UUID", JSON.stringify(await rows()) === JSON.stringify(["测试 RTSP 源"]));
  await q(setInput(".toolbar input[type=search]", ""));
  await q(clickText(".toolbar .segmented button", "推流设备"));
  check("类型筛选：推流设备", (await rows()).every(name => !name.startsWith("测试 GB")) && (await rows()).includes("测试 推流设备"));
  await q(clickText(".toolbar .segmented button", "全部"));
  await q(setInput(".toolbar select", "online"));
  check("状态筛选：在线", (await rows()).includes("园区东门 NVR") && !(await rows()).includes("测试 GB 设备"));
  await q(setInput(".toolbar select", "all"));
  await q(setInput(".toolbar input[type=search]", "不存在的设备"));
  check("无匹配空状态", (await q(text(".table-card"))).includes("没有符合条件的设备"));
  await q(clickText(".table-card .btn", "清除筛选"));
  check("清除筛选", (await rows()).length === before + 3);
  check("GB 设备编辑按钮禁用", await q("[...document.querySelectorAll('.table tbody tr')].find(r => r.innerText.includes('测试 GB 设备')).querySelector('[aria-label=编辑]').disabled"));

  // 编辑 RTSP：URL / 新密码 / 留空保留 / 清除
  const editRow = name => q(`[...document.querySelectorAll('.table tbody tr')].find(r => r.innerText.includes(${JSON.stringify(name)})).querySelector('[aria-label=编辑]').click()`);
  await editRow("测试 RTSP 源"); await b.sleep(200);
  check("编辑表单不回显密码", await q("document.querySelector('#device-dialog [name=password]').value") === "");
  await q(setInput("#device-dialog [name=url]", "rtsp://127.0.0.1:3/main"));
  await q(setInput("#device-dialog [name=password]", "second-pass"));
  await submit();
  check("保存 URL 与新密码", storedPassword(source.source_id) === "admin|second-pass" && (await api("/api/sources")).body.sources.find(item => item.source_id === source.source_id).url === "rtsp://127.0.0.1:3/main");
  check("提示与请求一致：更新密码", (await q(toasts)).includes("已保存，并更新了密码"));
  await editRow("测试 RTSP 源"); await b.sleep(200);
  await q(setInput("#device-dialog [name=name]", "测试 RTSP 源 改"));
  await submit();
  check("密码留空保留原密码", storedPassword(source.source_id) === "admin|second-pass" && (await q(toasts)).includes("已保存设备"));
  await waitFor(b, "document.querySelector('.table tbody').innerText.includes('测试 RTSP 源 改')");
  await editRow("测试 RTSP 源 改"); await b.sleep(200);
  await q(clickText("#device-dialog label.check", "清除已保存的密码"));
  check("勾选清除时禁用密码输入", await q("document.querySelector('#device-dialog [name=password]').disabled"));
  await submit();
  check("清除密码保留用户名", storedPassword(source.source_id) === "admin|" && (await q(toasts)).includes("清除了已保存的密码"));
  check("界面不出现密码", !(await q("document.body.innerText")).includes("second-pass"));

  // 编辑推流设备名称
  await waitFor(b, "document.querySelector('.table tbody').innerText.includes('测试 推流设备')");
  await editRow("测试 推流设备"); await b.sleep(200);
  await q(setInput("#device-dialog [name=name]", "测试 推流设备 改"));
  await submit();
  check("编辑推流设备名称", (await api(`/api/push-devices/${push.device_id}`)).body.name === "测试 推流设备 改");
  await b.shot(`${shots}/devices-1440.png`);

  // Esc 关闭；删除确认默认聚焦取消
  await openAdd();
  await q("document.getElementById('device-dialog').dispatchEvent(new Event('cancel', { cancelable: true })) && document.getElementById('device-dialog').close()");
  check("关闭添加对话框", !(await dialogOpen()));
  for (const name of ["测试 GB 设备", "测试 RTSP 源 改", "测试 推流设备 改"]) {
    await waitFor(b, `document.querySelector('.table tbody').innerText.includes(${JSON.stringify(name)})`);
    await q(`[...document.querySelectorAll('.table tbody tr')].find(r => r.innerText.includes(${JSON.stringify(name)})).querySelector('[aria-label=删除]').click()`);
    await b.sleep(200);
    if (name === "测试 GB 设备") check("删除确认默认聚焦取消", await q("document.activeElement.textContent") === "取消");
    await q("document.querySelector('#confirm-dialog [data-confirm]').click()");
    await waitFor(b, `!document.querySelector('.table tbody').innerText.includes(${JSON.stringify(name)})`, 5000);
    check(`删除 ${name}`, !(await q("document.querySelector('.table tbody').innerText")).includes(name));
  }
  check("删除后与接口一致", (await rows()).length === before);
  summary();
}
