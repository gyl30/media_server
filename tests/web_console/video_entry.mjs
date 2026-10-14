// 从设备详情进入视频中心播放：只创建这一路画面；离开后释放。
import { base, shots, check, clickText, waitFor, summary } from "./lib.mjs";
const PUSH = process.env.PUSH_DEVICE;
export default async function (b) {
  const q = js => b.eval(js);
  await b.viewport(1440, 900);
  await b.goto(base + `#devices/push/${PUSH}`, 2500);
  check("详情页提供在视频中心播放", await q("!document.querySelector('[data-key=play-center]').disabled"));
  await q("document.querySelector('[data-key=play-center]').click()");
  check("跳转到实时监控并播放这一路", await waitFor(b, "location.hash === '#monitor' && document.querySelectorAll('.tile.is-streaming').length === 1", 15000));
  check("只创建一个画面", await q("document.querySelectorAll('.grid-wrap .tile:not(.is-empty)').length") === 1);
  await b.sleep(1500);
  check("画面在解码", await q("document.querySelector('.tile.is-streaming video').getVideoPlaybackQuality().totalVideoFrames") > 10);
  await b.shot(`${shots}/video-single-real.png`);
  await q(clickText(".player-toolbar .segmented button", "单画面"));
  await b.sleep(1500);
  await b.shot(`${shots}/video-single-layout.png`);
  await b.viewport(390, 844);
  await b.sleep(800);
  check("390 无横向溢出", await q("document.documentElement.scrollWidth <= innerWidth"));
  await b.shot(`${shots}/390-video-real.png`, true);
  await q("history.back()");
  await b.sleep(800);
  check("返回详情后不在后台播放", await q("document.querySelectorAll('video').length") === 0);
  summary();
}
