// 离线精确编码与队列契约：真实浏览器采集另由 talk_probe 验证。
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { execFileSync, spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import vm from "node:vm";

let Processor;
const sent = [];
const scope = { sampleRate: 8000, currentTime: 0, Uint8Array,
  AudioWorkletProcessor: class { constructor() { this.port = { postMessage: message => sent.push(message), onmessage: null }; } },
  registerProcessor: (_, processor) => { Processor = processor; } };
vm.runInNewContext(readFileSync(new URL("../../signaling/web/talk-worklet.js", import.meta.url), "utf8"), scope);
const encoder = new Processor();
const pcm = Buffer.alloc(65536 * 2);
for (let index = 0; index < 65536; index++) pcm.writeInt16LE(index - 32768, index * 2);
const expected = execFileSync(process.env.FFMPEG_TEST_EXECUTABLE || "ffmpeg", ["-nostdin", "-hide_banner", "-loglevel", "error",
  "-f", "s16le", "-ar", "8000", "-ac", "1", "-i", "pipe:0", "-c:a", "pcm_alaw", "-f", "alaw", "pipe:1"], { input: pcm });
const actual = [];
for (let start = 0; start < 65600; start += 128) {
  const samples = Float32Array.from({ length: Math.min(128, 65600 - start) }, (_, offset) =>
    start + offset < 65536 ? (start + offset - 32768) / 32768 : 0);
  encoder.process([[samples]]);
  if (sent.length) {
    const frame = sent.shift();
    assert.equal(frame.packet.byteLength, 160);
    actual.push(Buffer.from(frame.packet));
    encoder.port.onmessage({ data: null });
  }
}
const encoded = Buffer.concat(actual).subarray(0, 65536);
const mismatches = [...encoded.keys()].filter(index => encoded[index] !== expected[index]);
console.log("FFmpeg code differences:", mismatches.length, "first PCM:", mismatches.slice(0, 8).map(index => index - 32768));
assert.deepEqual(encoded, expected);
const blocked = new Processor();
for (let index = 0; index < 100; index++) blocked.process([[new Float32Array(160)]]);
assert.equal(sent.length, 1, "no unbounded worklet messages while main thread stalls");
assert.ok(Buffer.from(sent.pop().packet).every(value => value === 0xd5));
blocked.port.onmessage({ data: null });
blocked.process([[new Float32Array(160).fill(0.5)]]);
assert.equal(sent.length, 1, "credit resumes with fresh input, not the skipped frames");
assert.ok(Buffer.from(sent.pop().packet).every(value => value !== 0xd5));
scope.sampleRate = 48000;
assert.throws(() => new Processor(), /8kHz/);
console.log("PASS: 65536 PCM values match FFmpeg; 160B framing, bounded credit, fresh resume, explicit sample rate");
for (const kind of ["missing", "directory"]) {
  const root = mkdtempSync(join(tmpdir(), "qt-talk-invalid-fixture-"));
  try {
    const result = spawnSync(process.execPath, [new URL("./cdp.mjs", import.meta.url).pathname], {
      env: { ...process.env, TMPDIR: root, WEB_CONSOLE_AUDIO_FIXTURE: kind === "missing" ? join(root, "missing.wav") : root },
      encoding: "utf8", timeout: 5000,
    });
    assert.equal(result.status, 1, "invalid audio fixture must fail before starting Chrome");
    assert.deepEqual(readdirSync(root), [], "invalid fixture must not leak a browser profile");
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}
console.log("PASS: missing/directory audio fixture rejects without creating a browser profile");
