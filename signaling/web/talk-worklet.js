class TalkEncoder extends AudioWorkletProcessor {
  constructor() {
    super();
    if (!Number.isFinite(sampleRate) || sampleRate < 8000) throw new Error("音频上下文采样率不支持");
    this.kernel = null;
    if (sampleRate !== 8000) {
      const half = Math.ceil(sampleRate / 200);
      this.kernel = new Float64Array(2 * half + 1);
      const cutoff = 3700 / sampleRate;
      let sum = 0;
      for (let index = -half; index <= half; index++) {
        const sinc = index === 0 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * index) / (Math.PI * index);
        const window = 0.42 + 0.5 * Math.cos(Math.PI * index / half) + 0.08 * Math.cos(2 * Math.PI * index / half);
        sum += this.kernel[index + half] = sinc * window;
      }
      for (let index = 0; index < this.kernel.length; index++) this.kernel[index] /= sum;
      this.history = new Float32Array(this.kernel.length + 1);
      this.position = 0;
      this.phase = 0;
    }
    this.packet = new Uint8Array(160);
    this.offset = 0;
    this.pending = false;
    this.port.onmessage = () => { this.pending = false; };
  }

  process(inputs) {
    const pcm = inputs[0]?.[0];
    if (!pcm) return true;
    for (let index = 0; index < pcm.length; index++) {
      if (!this.kernel) {
        this.encode(pcm[index], currentTime + (index + 1) / sampleRate);
        continue;
      }
      this.history[this.position] = pcm[index];
      this.phase += 8000;
      if (this.phase >= sampleRate) {
        this.phase -= sampleRate;
        const fraction = this.phase / 8000;
        let value = 0, cursor = this.position;
        for (let tap = 0; tap < this.kernel.length; tap++) {
          const previous = cursor === 0 ? this.history.length - 1 : cursor - 1;
          value += this.kernel[tap] * (this.history[cursor] + fraction * (this.history[previous] - this.history[cursor]));
          cursor = previous;
        }
        const delay = (this.kernel.length - 1) / (2 * sampleRate);
        this.encode(value, currentTime + (index + 1 - fraction) / sampleRate - delay);
      }
      this.position = (this.position + 1) % this.history.length;
    }
    return true;
  }

  encode(value, time) {
    let sample = Math.max(-32768, Math.min(32767, Math.round(value * 32768)));
    const mask = sample >= 0 ? 0xd5 : 0x55;
    sample = Math.abs(sample >> 2);
    let low = 0, high = 127;
    while (low < high) {
      const middle = (low + high) >> 1;
      const next = middle + 1;
      const level = middle < 16 ? middle * 16 + 8 : ((middle & 15) * 2 + 33) << ((middle >> 4) + 2);
      const nextLevel = next < 16 ? next * 16 + 8 : ((next & 15) * 2 + 33) << ((next >> 4) + 2);
      if (sample < ((level + nextLevel + 4) >> 3)) high = middle;
      else low = next;
    }
    this.packet[this.offset++] = low ^ mask;
    if (this.offset !== 160) return;
    this.offset = 0;
    if (this.pending) return;
    this.pending = true;
    this.port.postMessage({ packet: this.packet.buffer, time }, [this.packet.buffer]);
    this.packet = new Uint8Array(160);
  }
}

registerProcessor("gb-talk-encoder", TalkEncoder);
