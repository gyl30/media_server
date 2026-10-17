// 8kHz 单声道 PCM → PCMA；最多一个消息在主线程排队，迟到音频不补发。
class TalkEncoder extends AudioWorkletProcessor {
  constructor() {
    super();
    if (sampleRate !== 8000) throw new Error("对讲需要 8kHz 音频上下文");
    this.packet = new Uint8Array(160);
    this.offset = 0;
    this.pending = false;
    this.port.onmessage = () => { this.pending = false; };
  }

  process(inputs) {
    const pcm = inputs[0]?.[0];
    if (!pcm) return true;
    for (const value of pcm) {
      let sample = Math.max(-32768, Math.min(32767, Math.round(value * 32768)));
      const mask = sample >= 0 ? 0xd5 : 0x55;
      // 对128个正量化级二分取最近级；线性输入以4为单位，与项目FFmpeg的S16编码一致。
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
      if (this.offset !== 160) continue;
      this.offset = 0;
      if (this.pending) continue;
      this.pending = true;
      this.port.postMessage({ packet: this.packet.buffer, time: currentTime }, [this.packet.buffer]);
      this.packet = new Uint8Array(160);
    }
    return true;
  }
}

registerProcessor("gb-talk-encoder", TalkEncoder);
