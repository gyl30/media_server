export class TalkCapture {
  constructor(onState) {
    this.onState = onState;
    this.current = null;
  }

  start(deviceID) {
    if (this.current) return;
    const session = { deviceID, controller: new AbortController(), stream: null, audio: null, source: null,
      node: null, socket: null, request: null, cleanup: null, id: "", timer: null, progress: 0 };
    this.current = session;
    this.onState("preparing");
    void this.prepare(session).catch(error => {
      if (!session.controller.signal.aborted) void this.stop(error.message || "对讲建立失败");
    });
  }

  async prepare(session) {
    if (!/^[0-9]{20}$/.test(session.deviceID)) throw new Error("无效的国标设备");
    if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia || !window.AudioWorkletNode) {
      throw new Error("麦克风需要 HTTPS（或 localhost）及支持 AudioWorklet 的浏览器");
    }
    session.timer = setTimeout(() => void this.stop("麦克风或对讲准备超时"), 20000);
    const audio = session.audio = new AudioContext();
    const resumed = audio.resume().catch(() => {
      if (!session.controller.signal.aborted) void this.stop("浏览器未允许启动音频上下文");
    });
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1 }, video: false });
    if (session.controller.signal.aborted) {
      stream.getTracks().forEach(track => track.stop());
      return;
    }
    session.stream = stream;
    for (const track of stream.getTracks()) track.onended = () => void this.stop("麦克风已断开");
    await resumed;
    if (session.controller.signal.aborted) return;
    await audio.audioWorklet.addModule("/talk-worklet.js");
    if (session.controller.signal.aborted) return;
    session.source = audio.createMediaStreamSource(stream);
    session.node = new AudioWorkletNode(audio, "gb-talk-encoder", { channelCount: 1, channelCountMode: "explicit" });
    session.node.onprocessorerror = () => void this.stop("音频编码器已停止");
    session.request = (async () => {
      const response = await fetch(`/api/devices/${session.deviceID}/talk`, { method: "POST",
        headers: { "Content-Type": "application/json" }, body: "{}", signal: AbortSignal.timeout(15000) });
      const ticket = await response.json();
      if (!response.ok) throw new Error(`对讲建立失败：${ticket.error || response.status}`);
      if (!/^[0-9a-f]{64}$/.test(ticket.talk_id)) throw new Error("平台返回无效的对讲身份");
      session.id = ticket.talk_id;
      const upload = new URL(ticket.upload_url);
      if (!["ws:", "wss:"].includes(upload.protocol) || upload.username || upload.password || upload.search || upload.hash ||
          upload.pathname !== `/publish/gb28181/audio/${session.id}/${upload.pathname.split("/").pop()}` ||
          !/^[0-9a-f]{64}$/.test(upload.pathname.split("/").pop()) || (location.protocol === "https:" && upload.protocol !== "wss:")) {
        throw new Error("音频上传地址无效或未配置安全 WSS 网关");
      }
      return upload;
    })();
    const upload = await session.request;
    session.request = null;
    if (session.controller.signal.aborted) return;
    const socket = session.socket = new WebSocket(upload);
    socket.onerror = () => void this.stop("音频上传连接失败");
    socket.onclose = () => void this.stop("音频上传连接已断开");
    socket.onmessage = () => void this.stop("收到非预期的音频响应");
    clearTimeout(session.timer);
    session.timer = setTimeout(() => void this.stop("音频上传握手超时"), 5000);
    socket.onopen = () => {
      clearTimeout(session.timer);
      session.progress = performance.now();
      session.timer = setInterval(() => {
        if (performance.now() - session.progress > 3000 || audio.state !== "running") void this.stop("麦克风没有持续提供音频");
      }, 1000);
      session.node.port.onmessage = event => {
        if (session.controller.signal.aborted) return;
        const { packet, time } = event.data;
        if (socket.readyState !== WebSocket.OPEN || socket.bufferedAmount > 1660) {
          void this.stop("音频上传积压或连接已关闭");
          return;
        }
        if (packet.byteLength !== 160) {
          void this.stop("音频编码长度错误");
          return;
        }
        if (audio.currentTime - time < 0.2) {
          try {
            socket.send(packet);
          } catch {
            void this.stop("音频上传写入失败");
            return;
          }
          session.progress = performance.now();
        }
        session.node.port.postMessage(null);
      };
      session.source.connect(session.node);
      session.node.connect(audio.destination);
      this.onState("transmitting");
    };
  }

  release(session) {
    clearTimeout(session.timer);
    clearInterval(session.timer);
    if (session.socket) {
      session.socket.onopen = session.socket.onerror = session.socket.onclose = session.socket.onmessage = null;
      session.socket.close();
      session.socket = null;
    }
    if (session.node) {
      session.node.port.onmessage = null;
      session.node.onprocessorerror = null;
      session.node.disconnect();
      session.node.port.close();
      session.node = null;
    }
    session.source?.disconnect();
    session.source = null;
    session.stream?.getTracks().forEach(track => { track.onended = null; track.stop(); });
    session.stream = null;
    if (session.audio) {
      void session.audio.close().catch(() => {});
      session.audio = null;
    }
  }

  stop(error = "") {
    const session = this.current;
    if (!session) return Promise.resolve();
    if (session.cleanup) return session.cleanup;
    session.controller.abort();
    session.cleanup = Promise.resolve().then(async () => {
      try {
        await session.request;
      } catch { }
      if (session.id) {
        try {
          const response = await fetch(`/api/devices/${session.deviceID}/talk`, { method: "DELETE",
            headers: { "Content-Type": "application/json" }, body: JSON.stringify({ talk_id: session.id }),
            signal: AbortSignal.timeout(5000), keepalive: true });
          if (!response.ok && response.status !== 404) throw new Error(String(response.status));
        } catch {
          error += `${error ? "；" : ""}对讲清理未确认，平台将按期限回收`;
        }
      }
      if (this.current === session) {
        this.current = null;
        this.onState("stopped", error);
      }
    });
    this.release(session);
    this.onState("stopping");
    return session.cleanup;
  }
}
