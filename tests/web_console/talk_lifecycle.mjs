// 浏览器生命周期定向测试。HTTP票据是受控夹具，不将它冒充真实SIP/设备验收。
import { base, check, waitFor, summary } from "./lib.mjs";

export default async function (b) {
  await b.goto(base + "fixture", 100);
  await b.eval(`(async () => {
    const { TalkCapture } = await import('/talk.js');
    const originalFetch = window.fetch;
    const originalCapture = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
    window.__streams = []; window.__posts = 0; window.__deletes = []; window.__states = [];
    window.__mode = 'hold'; window.__deferMic = false; window.__denyMic = false; window.__denyGraph = false;
    navigator.mediaDevices.getUserMedia = async options => {
      if (window.__denyMic) throw new DOMException('controlled permission rejection', 'NotAllowedError');
      const stream = await originalCapture(options);
      window.__streams.push(stream);
      if (window.__deferMic) await new Promise(resolve => window.__releaseMic = resolve);
      return stream;
    };
    const originalSource = AudioContext.prototype.createMediaStreamSource;
    AudioContext.prototype.createMediaStreamSource = function(stream) {
      if (window.__denyGraph) {
        window.__rejectedAudio = this;
        throw new DOMException('controlled source graph rejection', 'NotSupportedError');
      }
      return originalSource.call(this, stream);
    };
    window.__ticket = digit => ({talk_id:digit.repeat(64), upload_url:
      'ws://' + location.host + '/publish/gb28181/audio/' + digit.repeat(64) + '/' + 'f'.repeat(64)});
    window.fetch = (url, options) => {
      if (!String(url).endsWith('/talk')) return originalFetch(url, options);
      if (options.method === 'DELETE') {
        window.__deletes.push(JSON.parse(options.body).talk_id);
        return Promise.resolve(new Response(null, {status:204}));
      }
      window.__posts++;
      const response = ticket => new Response(JSON.stringify(ticket), {status:201, headers:{'Content-Type':'application/json'}});
      if (window.__mode === 'hold') return new Promise(resolve => window.__releasePost = ticket => resolve(response(ticket)));
      if (window.__mode === 'invalid') return Promise.resolve(response({...window.__ticket('2'),upload_url:'http://invalid.test'}));
      return Promise.resolve(response(window.__ticket(window.__mode)));
    };
    document.querySelector('#start').onclick = () => {
      window.__states = [];
      window.__talk = new TalkCapture((state,error) => window.__states.push({state,error}));
      window.__talk.start('34020000001320000001');
    };
  })()`);
  const stopped = () => waitFor(b, "window.__states.at(-1)?.state === 'stopped'", 7000);
  const ended = () => b.eval("window.__streams.every(stream => stream.getTracks().every(track => track.readyState === 'ended'))");
  const click = () => b.click(50, 30);

  await b.eval("window.__deferMic = true");
  await click();
  check("延迟麦克风：真正打开过采集设备", await waitFor(b, "Boolean(window.__releaseMic)", 4000));
  await b.eval("window.__talk.stop()");
  await b.eval("window.__releaseMic(); window.__deferMic = false");
  check("取消后的迟到采集结果立即停止", await waitFor(b, "window.__streams.every(s => s.getTracks().every(t => t.readyState === 'ended'))", 2000));
  check("取消麦克风权限等待不申请设备授权", await b.eval("window.__posts === 0 && !window.__talk.current"));

  await click();
  check("真实采集准备后进入受控POST", await waitFor(b, "window.__posts === 1 && Boolean(window.__releasePost)", 4000));
  await b.eval("window.__stopping = window.__talk.stop(); true");
  check("POST等待期间停止立即回收麦克风", await ended());
  await b.eval("window.__releasePost(window.__ticket('1')); window.__stopping");
  check("迟到201按原准确ID删除且最终结束", await stopped() && await b.eval("window.__deletes.length === 1 && window.__deletes[0] === '1'.repeat(64) && !window.__talk.current"));
  await b.eval("Promise.all([window.__talk.stop(), window.__talk.stop()])");
  check("重复stop不重复DELETE", await b.eval("window.__deletes.length === 1"));

  await b.eval("window.__mode = 'invalid'");
  await click();
  check("无效上传URL拒绝并删除准确ID", await stopped() && await b.eval("window.__deletes.at(-1) === '2'.repeat(64) && window.__states.at(-1).error.includes('地址')"));
  check("无效票据后采集已结束", await ended());

  await b.eval("window.__denyMic = true; window.__beforePosts = window.__posts");
  await click();
  check("权限拒绝不创建对讲", await stopped() && await b.eval("window.__posts === window.__beforePosts"));
  await b.eval("window.__denyMic = false; window.__denyGraph = true; window.__beforePosts = window.__posts");
  await click();
  check("采集图不支持时不创建对讲", await stopped() && await b.eval("window.__posts === window.__beforePosts && window.__states.at(-1).error.includes('controlled source graph rejection')"));
  check("采集图拒绝后麦克风及context已关闭", await ended() && await waitFor(b, "window.__rejectedAudio.state === 'closed'", 2000));
  await b.eval("window.__denyGraph = false; window.__mode = '5'");
  await click();
  check("真实WS握手黑洞有界结束并清理", await stopped() && await b.eval("window.__states.at(-1).error.includes('握手超时') && window.__deletes.at(-1) === '5'.repeat(64)"));
  check("握手超时后采集已结束", await ended());

  await b.eval("window.__mode = '6'");
  await click();
  check("真实WS远端收包后关闭触发清理", await stopped() && await b.eval("window.__states.some(s => s.state === 'transmitting') && window.__deletes.at(-1) === '6'.repeat(64)"));
  check("远端关闭后全部采集已结束", await ended());
  await b.eval("window.__mode = '7'");
  await click();
  check("持续采集WS真实输出", await waitFor(b, "window.__states.at(-1)?.state === 'transmitting'", 4000));
  await b.sleep(200);
  const before = await b.eval("fetch('/fixture/packets').then(r => r.json())");
  await b.eval("{ const end = performance.now() + 400; while (performance.now() < end) {} }");
  const resumed = await b.eval("fetch('/fixture/packets').then(r => r.json())");
  await b.sleep(150);
  const after = await b.eval("fetch('/fixture/packets').then(r => r.json())");
  check("400ms主线程卡顿后不补发积压音频", after.count - resumed.count <= 11 && after.max_gap_ms >= 250 && before.count >= 3,
    JSON.stringify({before,resumed,after}));
  await b.eval("window.__talk.stop()");
  check("卡顿试验后回收采集与上传", await stopped() && await ended());
  summary();
}
