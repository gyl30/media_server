// 390px 基础交互：抽屉导航、设备卡片、添加弹窗、实时监控播放与离开释放。需要 PUSH_DEVICE（正在推流）。
import { base, shots, check, clickText, waitFor, summary } from "./lib.mjs";
export default async function (b) {
  const q = js => b.eval(js);
  await b.viewport(390, 844);
  await b.goto(base + "#overview", 2500);
  check("窄屏隐藏侧边栏", await q("document.querySelector('.sidebar').getBoundingClientRect().right <= 0"));
  await q("document.getElementById('menu-button').click()"); await b.sleep(300);
  check("打开导航抽屉", await q("document.getElementById('shell').classList.contains('nav-open') && document.querySelector('.sidebar').getBoundingClientRect().left === 0"));
  await b.shot(`${shots}/390-drawer.png`);
  await q("document.getElementById('shade').click()"); await b.sleep(300);
  check("点击遮罩关闭抽屉", !(await q("document.getElementById('shell').classList.contains('nav-open')")));
  await q("document.getElementById('menu-button').click()"); await b.sleep(200);
  await q(clickText(".nav-item", "设备管理")); await b.sleep(600);
  check("抽屉导航后关闭并进入设备管理", await q("location.hash") === "#devices" && !(await q("document.getElementById('shell').classList.contains('nav-open')")));
  check("设备列表为卡片且无横向溢出", await q("document.documentElement.scrollWidth <= innerWidth && getComputedStyle(document.querySelector('.table thead')).display === 'none'"));
  await q(clickText(".page-head .btn", "添加设备")); await b.sleep(300);
  check("添加弹窗在视口内", await q("(() => { const r = document.getElementById('device-dialog').getBoundingClientRect(); return r.left >= 0 && r.right <= innerWidth; })()"));
  await b.shot(`${shots}/390-add-dialog.png`);
  await q("document.getElementById('device-dialog').close()");
  await q("location.hash = '#monitor'"); await b.sleep(1500);
  await q(`document.querySelector('[data-key="play:push:${process.env.PUSH_DEVICE}"]').click()`);
  check("窄屏播放推流画面", await waitFor(b, "document.querySelector('.tile.is-streaming video')?.getVideoPlaybackQuality().totalVideoFrames > 10", 15000));
  check("实时监控无横向溢出", await q("document.documentElement.scrollWidth <= innerWidth"));
  await b.shot(`${shots}/390-monitor.png`, true);
  await q("document.getElementById('menu-button').click()"); await b.sleep(200);
  await q(clickText(".nav-item", "控制台")); await b.sleep(600);
  check("离开实时监控后无视频元素", await q("document.querySelectorAll('video').length") === 0);
  summary();
}
