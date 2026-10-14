// 设备详情播放地址（真实接口与真实媒体）：签发、共用一次性 token 的实际效果、刷新、过期、
// 乱序与媒体流变化、GB 点播前不签发、RTSP 拉流按会话签发。需要 PUSH_DEVICE（正在推流的推流设备）。
import { execFileSync } from "node:child_process";
import { base, shots, check, clickText, text, toasts, api, waitFor, summary } from "./lib.mjs";

const GB = "34020000001320000101";
const CHANNEL = "34020000001310000001";
const PUSH = process.env.PUSH_DEVICE;

// 用 ffprobe 实际发起一次播放连接，返回是否成功拿到视频流信息。
function probe(url) {
  try {
    const transport = url.startsWith("rtsp://") ? ["-rtsp_transport", "tcp"] : [];
    const out = execFileSync("ffprobe", ["-v", "error", ...transport, "-rw_timeout", "4000000", "-select_streams", "v", "-show_entries", "stream=codec_name", "-of", "csv=p=0", url], { timeout: 12000, stdio: ["ignore", "pipe", "pipe"] }).toString();
    return out.includes("h264");
  } catch {
    return false;
  }
}

export default async function (b) {
  const q = js => b.eval(js);
  const urls = () => q("[...document.querySelectorAll('.urls-panel .url-value')].map(e => e.textContent)");
  const token = url => url.match(/\/([0-9a-f]{64})(?:\.flv|\/index\.m3u8)?$/)?.[1];
  await b.viewport(1440, 900);

  // 推流设备：签发 5 个地址，全部共用一个 token
  const stream = (await api(`/api/push-devices/${PUSH}`)).body.token;
  await b.goto(base + `#devices/push/${PUSH}`, 1500);
  check("推流中设备打开详情即签发 5 个地址", await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 5", 5000));
  let list = await urls();
  check("地址属于当前媒体流", list.every(url => url.includes(`/${stream}/`)));
  check("五个地址共用一个 token（与当前服务端一致）", new Set(list.map(token)).size === 1 && token(list[0]));
  check("界面说明共用授权且不展示 token 字段", (await q(text(".urls-panel"))).includes("共用同一个一次性授权") && !(await q(text(".urls-panel"))).toLowerCase().includes("token"));
  check("面板无多余的 null / undefined 文本", !/\b(null|undefined)\b/.test(await q(text(".urls-panel"))));
  check("显示组级倒计时", /\d+s\s*内可发起新连接/.test(await q(text(".urls-meta"))));
  await b.shot(`${shots}/detail-push-playback.png`);

  // 共用 token 的实际效果：使用 FLV 后，同组 RTSP 地址被拒绝
  check("同组 HTTP-FLV 地址可播放", probe(list[2]));
  check("同组 RTSP 地址随之失效（共用授权）", !probe(list[0]));
  await q(clickText(".urls-panel .card-head .btn", "全部刷新"));
  await waitFor(b, `(() => { const u = document.querySelector('.urls-panel .url-value')?.textContent; return u && !u.includes(${JSON.stringify(token(list[0]))}); })()`, 5000);
  const refreshed = await urls();
  check("全部刷新得到新 token", token(refreshed[0]) !== token(list[0]) && (await q(toasts)).includes("已重新申请播放地址"));
  check("刷新后的 RTSP 地址可播放", probe(refreshed[0]));

  // 复制：成功与失败
  await q("navigator.clipboard.writeText = async () => {}");
  await q("document.querySelector('.urls-panel .copy-btn').click()"); await b.sleep(150);
  check("复制成功提示", (await q(toasts)).includes("已复制 RTSP 播放地址"));
  await q("navigator.clipboard.writeText = async () => { throw new Error('denied'); }; document.execCommand = () => false");
  await q("document.querySelectorAll('.urls-panel .copy-btn')[1].click()"); await b.sleep(150);
  check("复制失败提示", (await q(toasts)).includes("复制失败"));

  // 重新进入详情签发新地址
  await b.goto(base + "#devices", 600);
  await b.goto(base + `#devices/push/${PUSH}`, 400);
  await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 5", 5000);
  check("重新进入详情得到新地址", token((await urls())[0]) !== token(refreshed[0]));

  // 乱序：慢响应期间离开到另一台设备，旧响应不能写入新页面
  await q("window.__delay = 0; const original = window.fetch; window.fetch = (url, options) => String(url).includes('/api/play') ? new Promise(resolve => setTimeout(() => resolve(original(url, options)), window.__delay)) : original(url, options)");
  await q("window.__delay = 2000");
  await q(clickText(".urls-panel .card-head .btn", "全部刷新"));
  await q(`location.hash = '#devices/gb/${GB}'`);
  await b.sleep(2600);
  check("离开后旧响应未写入 GB 详情", (await urls()).length === 0 && (await q(text(".urls-panel"))).includes("媒体未就绪"));

  // GB：打开详情不点播；点播后按 live 签发；停止后清空
  check("GB 未点播时不创建 live", !(await api(`/api/devices/${GB}/channels`)).body.channels[0].live);
  await q("window.__delay = 0");
  await q(clickText(".urls-panel .btn", "开始点播"));
  check("点播后签发地址", await waitFor(b, "document.querySelectorAll('.urls-panel .url-value').length === 5", 15000));
  const live = (await api(`/api/devices/${GB}/channels`)).body.channels[0].live;
  check("地址属于该通道的 live", (await urls()).every(url => url.includes(`/${live.live_id}/`)));
  // signaling 的 streaming 早于媒体侧完成 PS 解析，稍等首个关键帧后再实际播放。
  await b.sleep(2000);
  check("GB 点播 RTSP 地址可播放", probe((await urls())[0]));
  await b.shot(`${shots}/detail-gb-playback.png`);

  // 慢响应期间媒体流结束：迟到的地址不展示
  await q("window.__delay = 1500");
  await q(clickText(".urls-panel .card-head .btn", "全部刷新"));
  await api(`/api/lives/${live.live_id}`, { method: "DELETE" });
  await b.sleep(3500);
  check("停止点播后不保留旧地址", (await urls()).length === 0 && (await q(text(".urls-panel"))).includes("开始点播"));
  await q("window.__delay = 0");

  // 申请时媒体流已结束：错误态（服务端 409 not_running）
  await api(`/api/push-devices/${PUSH}/stop`, { method: "POST" });
  await b.goto(base + "#devices", 300);
  await q(`location.hash = '#devices/push/${PUSH}'`);
  await b.sleep(400);
  const failed = await q(text(".urls-panel"));
  console.log("INFO 推流停止后打开详情：", failed.replace(/\n/g, " ").slice(0, 120));
  check("媒体流结束后显示未就绪或申请失败，不展示地址", (await urls()).length === 0 && (failed.includes("设备未推流") || failed.includes("申请失败")));
  check("快照更新后显示设备未推流", await waitFor(b, "document.querySelector('.urls-panel').innerText.includes('设备未推流')", 6000));
  summary();
}
