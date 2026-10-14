// RTSP 拉流源的播放地址（真实媒体）与 60 秒过期。需要 PUSH_DEVICE（正在推流），以其 RTSP 播放地址作为拉流源。
import { base, shots, check, clickText, text, api, waitFor, summary } from "./lib.mjs";

const PUSH = process.env.PUSH_DEVICE;

export default async function (b) {
  const q = js => b.eval(js);
  const urls = () => q("[...document.querySelectorAll('.urls-panel .url-value')].map(e => e.textContent)");
  await b.viewport(1440, 900);
  for (const source of (await api("/api/sources")).body.sources.filter(item => item.stream_name === "拉流测试 源")) {
    await api(`/api/sources/${source.source_id}`, { method: "DELETE" });
  }
  const ticket = (await api("/api/play", { method: "POST", body: { source: { push_device_id: PUSH } } })).body;
  const source = (await api("/api/sources", { method: "POST", body: { stream_name: "拉流测试 源", url: ticket.rtsp_url } })).body;
  await b.goto(base + `#devices/rtsp/${source.source_id}`, 1500);
  check("添加即自动拉流", source.desired_state === "running" && Boolean(source.session), JSON.stringify(source));
  check("取流成功后签发地址", await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 5", 15000));
  const session = (await api("/api/sources")).body.sources.find(item => item.source_id === source.source_id).session;
  check("地址属于拉流会话的媒体流", session?.state === "created" && (await urls()).every(url => url.includes(`/${session.stream_id}/`)));
  await b.shot(`${shots}/detail-rtsp-playback.png`);

  // 60 秒过期（--token-ttl 默认值）
  await b.sleep(48000);
  check("最后 10 秒倒计时变为提醒色", await waitFor(b, "!!document.querySelector('.group-ttl.low')", 5000), await q(text(".urls-meta")));
  await b.shot(`${shots}/detail-ttl-low.png`);
  await b.sleep(12000);
  check("过期后整体提示且禁用复制", (await q(text(".urls-panel"))).includes("本组地址已过期") && await q("[...document.querySelectorAll('.urls-panel .copy-btn')].every(b => b.disabled)"));
  await b.shot(`${shots}/detail-expired.png`);
  await q(clickText(".group-notice .btn", "重新申请"));
  check("重新申请后恢复可用", await waitFor(b, "!document.querySelector('[data-expired]') && document.querySelectorAll('.urls-panel .url-value').length === 5", 5000));

  await q(clickText(".detail-head .btn", "停止拉流"));
  await b.sleep(150);
  await q("document.querySelector('#confirm-dialog [data-confirm]').click()");
  check("停止取流后清空地址", await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 0", 8000));
  await api(`/api/sources/${source.source_id}`, { method: "DELETE" });
  summary();
}
