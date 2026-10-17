// 离线精确编码与队列契约：真实浏览器采集另由 talk_probe 验证。
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { execFileSync, spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import vm from "node:vm";

const source = readFileSync(new URL("../../signaling/web/talk-worklet.js", import.meta.url), "utf8");
function processor(rate) {
  let Processor;
  const sent = [];
  const scope = { sampleRate: rate, currentTime: 0, Uint8Array,
    AudioWorkletProcessor: class { constructor() { this.port = {
      postMessage: (message, transfer) => sent.push(structuredClone(message, { transfer })), onmessage: null,
    }; } },
    registerProcessor: (_, implementation) => { Processor = implementation; } };
  vm.runInNewContext(source, scope);
  return { encoder: new Processor(), sent, scope };
}
const { encoder, sent } = processor(8000);
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
const { encoder: blocked, sent: pending } = processor(8000);
for (let index = 0; index < 100; index++) blocked.process([[new Float32Array(160)]]);
assert.equal(pending.length, 1, "no unbounded worklet messages while main thread stalls");
assert.ok(Buffer.from(pending.pop().packet).every(value => value === 0xd5));
blocked.port.onmessage({ data: null });
blocked.process([[new Float32Array(160).fill(0.5)]]);
assert.equal(pending.length, 1, "credit resumes with fresh input, not the skipped frames");
assert.ok(Buffer.from(pending.pop().packet).every(value => value !== 0xd5));
console.log("PASS: 8kHz passthrough, 65536 PCM values match FFmpeg, bounded credit and fresh resume");

function render(rate, samples, sizes = [128]) {
  const state = processor(rate);
  const packets = [];
  let previousTime;
  let offset = 0, chunk = 0;
  while (offset < samples.length) {
    const end = Math.min(samples.length, offset + sizes[chunk++ % sizes.length]);
    state.scope.currentTime = offset / rate;
    state.encoder.process([[samples.subarray(offset, end)]]);
    while (state.sent.length) {
      const message = state.sent.shift();
      assert.equal(message.packet.byteLength, 160);
      assert.ok(Number.isFinite(message.time));
      assert.ok(message.time <= end / rate && message.time >= (end / rate) - 0.04, "packet timestamp follows fresh input");
      if (previousTime !== undefined) assert.ok(Math.abs(message.time - previousTime - 0.02) < 1e-8, "packet media clock advances 20ms across input blocks");
      previousTime = message.time;
      packets.push(Buffer.from(message.packet));
      state.encoder.port.onmessage({ data: null });
    }
    offset = end;
  }
  return { bytes: Buffer.concat(packets), encoder: state.encoder };
}

function decode(bytes) {
  return Float64Array.from(bytes, byte => {
    const value = byte ^ 0x55;
    const segment = (value >> 4) & 7;
    let magnitude = (value & 15) << 4;
    magnitude = segment === 0 ? magnitude + 8 : (magnitude + 0x108) << (segment - 1);
    return (value & 0x80 ? magnitude : -magnitude) / 32768;
  });
}

function tone(rate, frequency, seconds = 1, amplitude = 0.25) {
  return Float32Array.from({ length: rate * seconds }, (_, index) => amplitude * Math.sin(2 * Math.PI * frequency * index / rate));
}

function signal(bytes, frequency) {
  const pcm = decode(bytes).subarray(800); // 固定去掉首100ms的滤波启动区间，后续全部参与。
  const mean = pcm.reduce((sum, value) => sum + value, 0) / pcm.length;
  let sine = 0, cosine = 0, energy = 0;
  for (let index = 0; index < pcm.length; index++) {
    const value = pcm[index] - mean;
    const angle = 2 * Math.PI * frequency * index / 8000;
    sine += value * Math.sin(angle);
    cosine += value * Math.cos(angle);
    energy += value * value;
  }
  const rms = Math.sqrt(energy / pcm.length);
  return { rms, correlation: rms === 0 ? 0 : Math.SQRT2 * Math.hypot(sine, cosine) / pcm.length / rms };
}

const failures = [];
for (const rate of [0, 7999, NaN, Infinity]) assert.throws(() => processor(rate));
for (const rate of [44100, 48000]) {
  try {
    const input = tone(rate, 1000, 10);
    const regular = render(rate, input);
    assert.equal(regular.bytes.length, 80000, "ten seconds produce exactly 80000 PCMA samples");
    assert.equal(regular.encoder.offset, 0, "no blockwise rounding drift");
    const partitioned = render(rate, input, [17, 131, 1, 257, 63]);
    assert.deepEqual(partitioned.bytes, regular.bytes, "filter history and fractional phase survive arbitrary block boundaries");
    const pass = signal(regular.bytes, 1000);
    assert.ok(pass.rms > 0.17 && pass.rms < 0.185 && pass.correlation > 0.995, JSON.stringify({ rate, pass }));
    const speech = signal(render(rate, tone(rate, 3400)).bytes, 3400);
    assert.ok(speech.rms > 0.16 && speech.rms < 0.19 && speech.correlation > 0.99, JSON.stringify({ rate, speech }));
    const stops = [];
    for (const frequency of [4000, 4100, 6000, 10000]) {
      const stop = signal(render(rate, tone(rate, frequency)).bytes, Math.abs(8000 - frequency));
      assert.ok(stop.rms < pass.rms * 0.0032, `stopband >=49.9dB after PCMA: ${JSON.stringify({ rate, frequency, stop })}`);
      stops.push({ frequency, rms: stop.rms });
    }

    const state = processor(rate);
    const reference = processor(rate);
    let inputFrames = 0, compared = 0;
    // 不发credit的这段仍必须推进滤波/相位；恢复与始终消费的参考实例比较。
    for (let block = 0; block < 500; block++) {
      const size = [113, 128, 137][block % 3];
      const input = Float32Array.from({ length: size }, (_, index) =>
        block < 180 ? 0 : 0.25 * Math.sin(2 * Math.PI * 1000 * (inputFrames + index) / rate));
      state.scope.currentTime = reference.scope.currentTime = inputFrames / rate;
      state.encoder.process([[input]]);
      reference.encoder.process([[input]]);
      assert.ok(state.sent.length <= 1, "credit bounds the pending worklet message");
      if (block === 200) {
        assert.equal(state.sent.length, 1);
        state.sent.shift();
        state.encoder.port.onmessage({ data: null });
      } else if (block > 200 && state.sent.length) {
        assert.equal(reference.sent.length, 1);
        const actual = state.sent.shift(), expected = reference.sent[0];
        assert.deepEqual(Buffer.from(actual.packet), Buffer.from(expected.packet), "credit recovery uses current filter history and phase");
        assert.ok(actual.time > 0.5, "no old packet timestamp after recovery");
        state.encoder.port.onmessage({ data: null });
        compared++;
      }
      if (reference.sent.length) {
        reference.sent.shift();
        reference.encoder.port.onmessage({ data: null });
      }
      inputFrames += size;
    }
    assert.ok(compared > 20, "sustained fresh packets after credit recovery");
    console.log(`PASS: ${rate}Hz → 8kHz; 80000 samples/500 packets, arbitrary partitions, 20ms packet clock, tone=${JSON.stringify(pass)}, speech3400=${JSON.stringify(speech)}, stopband=${JSON.stringify(stops)}, bounded fresh credit`);
  } catch (error) {
    console.error(`FAIL: native ${rate}Hz:`, error.message);
    failures.push(error);
  }
}
if (failures.length) throw new AggregateError(failures, "native-rate worklet validation failed");
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
