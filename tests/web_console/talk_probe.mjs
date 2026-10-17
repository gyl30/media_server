// 真实 Chrome mic fixture→AudioWorklet→WS；由 Go 实际 SIP/RTP peer 另行验证媒体与清理。
import { base, waitFor } from "./lib.mjs";

export default async function (browser) {
  const device = process.env.WEB_CONSOLE_TALK_DEVICE;
  if (!/^[0-9]{20}$/.test(device || "") || !process.env.WEB_CONSOLE_AUDIO_FIXTURE) throw new Error("explicit talk device and WAV fixture required");
  await browser.goto(base + `#devices/gb/${device}`, 200);
  if (!await waitFor(browser, "document.querySelector('[data-key=\"talk\"]:not(:disabled)')", 5000)) throw new Error("device detail talk button unavailable");
  await browser.eval(`(async () => {
    const { TalkCapture } = await import('/talk.js');
    window.__talkStates = [];
    const start = TalkCapture.prototype.start;
    TalkCapture.prototype.start = function(device) {
      window.__talk = this;
      const onState = this.onState;
      this.onState = (state, error) => { window.__talkStates.push({state, error}); onState(state, error); };
      return start.call(this, device);
    };
  })()`);
  const point = await browser.eval("(() => { const r = document.querySelector('[data-key=\"talk\"]').getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2}; })()");
  await browser.click(point.x, point.y);
  if (!await waitFor(browser, "window.__talkStates.some(item => item.state === 'transmitting' || item.state === 'stopped')", 18000)) {
    throw new Error("Web talk never reached a terminal or transmitting state");
  }
  const active = await browser.eval("window.__talkStates.at(-1)");
  if (active.state !== "transmitting") throw new Error(JSON.stringify(active));
  const transport = await browser.eval("({secure:window.isSecureContext, page:location.protocol, upload:new URL(window.__talk.current.socket.url).protocol})");
  if (!transport.secure || (process.env.GB_AUDIO_TEST_TLS_CERT && (transport.page !== "https:" || transport.upload !== "wss:"))) {
    throw new Error("secure capture/transport contract was not met");
  }
  console.log("capture transport:", JSON.stringify(transport));
  await browser.sleep(2000);
  let background;
  if (process.env.WEB_CONSOLE_TALK_STOP === "route") await browser.eval("location.hash = '#overview'");
  else if (process.env.WEB_CONSOLE_TALK_STOP === "hidden") {
    background = await browser.hide();
    if (!await waitFor(browser, "document.hidden", 2000)) throw new Error("owned tab never became hidden");
  } else await browser.eval("document.querySelector('[data-key=\"talk\"]').click()");
  if (!await waitFor(browser, "window.__talkStates.at(-1)?.state === 'stopped'", 6000)) throw new Error("detail talk stop did not finish");
  const result = await browser.eval("({states:window.__talkStates, released:!window.__talk.current})");
  if (!result.released || result.states.some(item => item.error)) throw new Error(JSON.stringify(result));
  if (background) await browser.show(background);
  console.log("PASS: real Chrome capture/worklet/WS stopped and exact owner DELETE confirmed");
}
