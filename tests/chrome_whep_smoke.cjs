const { chromium } = require('playwright');

async function main() {
  const [signaling_url, source_id] = process.argv.slice(2);
  if (!signaling_url || !source_id) throw new Error('usage: node chrome_whep_smoke.cjs signaling_url source_id');
  const browser = await chromium.launch({
    executablePath: process.env.CHROME_BIN || '/usr/bin/google-chrome',
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required'],
  });
  try {
    const page = await browser.newPage();
    const deletes = [];
    page.on('response', response => {
      if (response.request().method() === 'DELETE') deletes.push(response.status());
    });
    await page.goto(signaling_url);
    await page.evaluate(async source_id => {
      const { WHEPPreview } = await import('/whep.js');
      const video = document.createElement('video');
      video.muted = true;
      video.autoplay = true;
      document.body.append(video);
      window.smoke_preview = new WHEPPreview(video, () => {});
      await window.smoke_preview.start({ source_id }, 'Chrome smoke');
      window.smoke_peer = window.smoke_preview.current.peer;
    }, source_id);
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      const ready = await page.evaluate(async () => {
        const reports = [...(await window.smoke_peer.getStats()).values()];
        return reports.some(r => r.type === 'inbound-rtp' && r.kind === 'video' && r.framesDecoded >= 30) &&
          reports.some(r => r.type === 'inbound-rtp' && r.kind === 'audio' && r.packetsReceived >= 30);
      });
      if (ready) break;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    const result = await page.evaluate(async () => {
      const peer = window.smoke_peer;
      const stats = await peer.getStats();
      const reports = [...stats.values()];
      const inbound = reports.filter(r => r.type === 'inbound-rtp').map(r => ({
        kind: r.kind, codec: stats.get(r.codecId)?.mimeType,
        packets: r.packetsReceived, frames_decoded: r.framesDecoded,
        bytes: r.bytesReceived,
      }));
      const dtls = reports.filter(r => r.type === 'transport').map(r => r.dtlsState);
      const result = { ice: peer.iceConnectionState, connection: peer.connectionState, dtls, inbound };
      await window.smoke_preview.stop();
      result.closed = peer.connectionState === 'closed';
      result.video_detached = window.smoke_preview.video.srcObject === null;
      return result;
    });
    result.browser = await browser.version();
    result.delete_statuses = deletes;
    console.log(JSON.stringify(result));
    if (!['connected', 'completed'].includes(result.ice) || !result.dtls.includes('connected') ||
        !result.inbound.some(r => r.codec === 'video/H264' && r.frames_decoded >= 30) ||
        !result.inbound.some(r => r.codec === 'audio/opus' && r.packets >= 30) ||
        !result.closed || !result.video_detached || !deletes.includes(204)) {
      throw new Error('Chrome WHEP interoperability check failed');
    }
  } finally {
    await browser.close();
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
