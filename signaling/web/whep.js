import { api } from "/api.js";

class WHEPError extends Error {
  constructor(status, code) {
    super(code);
    this.name = "WHEPError";
    this.status = status;
    this.code = code;
  }
}

function abortError() {
  return new DOMException("Preview stopped", "AbortError");
}

function waitForDelay(milliseconds, signal) {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(abortError());
      return;
    }
    const finish = (callback) => {
      window.clearTimeout(timer);
      signal.removeEventListener("abort", cancelled);
      callback();
    };
    const cancelled = () => finish(() => reject(abortError()));
    const timer = window.setTimeout(() => finish(resolve), milliseconds);
    signal.addEventListener("abort", cancelled, { once: true });
  });
}

function waitForICEGathering(peer, signal) {
  if (peer.iceGatheringState === "complete") {
    return Promise.resolve();
  }
  return new Promise((resolve, reject) => {
    const finish = () => {
      peer.removeEventListener("icegatheringstatechange", changed);
      signal.removeEventListener("abort", cancelled);
    };
    const changed = () => {
      if (peer.iceGatheringState === "complete") {
        finish();
        resolve();
      }
    };
    const cancelled = () => {
      finish();
      reject(abortError());
    };
    peer.addEventListener("icegatheringstatechange", changed);
    signal.addEventListener("abort", cancelled, { once: true });
  });
}

async function deleteResource(resourceURL, keepalive = false) {
  if (!resourceURL) {
    return;
  }
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(), 3000);
  try {
    const response = await fetch(resourceURL, { method: "DELETE", signal: controller.signal, keepalive });
    if (!response.ok && response.status !== 404) {
      throw new WHEPError(response.status, `whep_delete_${response.status}`);
    }
  } finally {
    window.clearTimeout(timer);
  }
}

async function postOffer(session, offer, retryNotReady) {
  while (!session.cancelled) {
    const response = await fetch(session.whepURL, {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: offer,
    });
    if (response.status === 409 && retryNotReady) {
      await response.text();
      const seconds = Number.parseInt(response.headers.get("Retry-After") || "1", 10);
      const delay = Number.isFinite(seconds) && seconds > 0 ? Math.min(seconds, 5) : 1;
      await waitForDelay(delay * 1000, session.controller.signal);
      continue;
    }
    if (response.status !== 201) {
      await response.text();
      throw new WHEPError(response.status, `whep_create_${response.status}`);
    }
    const location = response.headers.get("Location");
    if (location) session.resourceURL = new URL(location, session.whepURL).toString();
    const contentType = response.headers.get("Content-Type") || "";
    const answer = await response.text();
    if (!location || !contentType.toLowerCase().startsWith("application/sdp") || !answer) {
      throw new WHEPError(response.status, "invalid_whep_response");
    }
    return answer;
  }
  throw abortError();
}

export class WHEPPreview {
  constructor(video, onState) {
    this.video = video;
    this.onState = onState;
    this.current = null;
    this.generation = 0;
  }

  emit(state, session = this.current, error = "") {
    this.onState({
      canStop: Boolean(session && this.current === session),
      state,
      target: session ? session.label : "",
      liveID: session?.liveID || "",
      needsPlaybackGesture: session?.needsPlaybackGesture || false,
      error,
    });
  }

  async start(target, label) {
    const generation = ++this.generation;
    await this.closeCurrent(generation);
    if (generation !== this.generation) {
      return null;
    }
    const session = {
      cancelled: false,
      controller: new AbortController(),
      generation,
      label,
      target,
      liveID: "",
      mediaStream: new MediaStream(),
      peer: null,
      resourceURL: "",
      streaming: false,
      whepURL: "",
    };
    this.current = session;
    this.emit("preparing", session);
    try {
      session.peer = new RTCPeerConnection();
      session.peer.addTransceiver("video", { direction: "recvonly" });
      session.peer.addTransceiver("audio", { direction: "recvonly" });
      session.peer.addEventListener("track", (event) => {
        if (!this.isCurrent(session)) return;
        if (![...session.mediaStream.getTracks()].some((track) => track.id === event.track.id)) {
          session.mediaStream.addTrack(event.track);
        }
        if (this.current === session) {
          this.video.srcObject = session.mediaStream;
          void this.video.play().catch(error => {
            if (this.isCurrent(session) && error.name === "NotAllowedError") {
              session.needsPlaybackGesture = true;
              this.emit(session.streaming ? "streaming" : "negotiating", session);
            }
          });
        }
        event.track.addEventListener("ended", () => {
          if (session.mediaStream.getTracks().every(track => track.readyState === "ended")) {
            void this.end(session);
          }
        });
      });
      session.peer.addEventListener("connectionstatechange", () => {
        if (this.current !== session) {
          return;
        }
        if (session.peer.connectionState === "connected") {
          this.markStreaming(session);
        } else if (["failed", "closed"].includes(session.peer.connectionState)) {
          void this.end(session);
        }
      });

      const offer = await session.peer.createOffer();
      await session.peer.setLocalDescription(offer);
      await waitForICEGathering(session.peer, session.controller.signal);
      if (!this.isCurrent(session) || !session.peer.localDescription) {
        this.closeLocal(session);
        return null;
      }
      this.emit("negotiating", session);
      let answer;
      for (let attempt = 0; attempt < 2; attempt += 1) {
        const ticket = target.source_id
          ? await api.startPreview(target, session.controller.signal)
          : await api.playChannel(target.device_id, target.channel_id, session.controller.signal);
        if (!this.isCurrent(session)) return null;
        session.whepURL = ticket.whep_url;
        session.liveID = ticket.live_id || "";
        this.emit("negotiating", session);
        try {
          answer = await postOffer(session, session.peer.localDescription.sdp, Boolean(target.source_id));
          break;
        } catch (error) {
          if (target.source_id || ![404, 409].includes(error.status) || attempt !== 0) throw error;
          await waitForDelay(1500, session.controller.signal);
        }
      }
      if (!this.isCurrent(session)) {
        await deleteResource(session.resourceURL, true);
        this.closeLocal(session);
        return null;
      }
      await session.peer.setRemoteDescription({ type: "answer", sdp: answer });
      if (!this.isCurrent(session)) {
        await deleteResource(session.resourceURL, true);
        this.closeLocal(session);
        return null;
      }
      if (session.peer.connectionState === "connected") {
        this.markStreaming(session);
      }
      return true;
    } catch (error) {
      if (session.resourceURL) {
        try {
          await deleteResource(session.resourceURL, session.cancelled);
        } catch {
        }
      }
      this.closeLocal(session);
      if (this.current === session) {
        this.current = null;
        this.emit(session.cancelled ? "idle" : "failed", session, session.cancelled ? "" : error.code || "network_error");
      }
      if (session.cancelled || error.name === "AbortError") {
        return null;
      }
      throw error;
    }
  }

  async closeViewer() {
    const generation = ++this.generation;
    return this.closeCurrent(generation);
  }

  async closeCurrent(generation) {
    const session = this.current;
    if (!session) {
      if (generation === this.generation) this.emit("idle", null);
      return;
    }
    this.current = null;
    session.cancelled = true;
    session.controller.abort();
    this.emit("stopping", session);
    const deletion = deleteResource(session.resourceURL);
    this.closeLocal(session);
    let deleteError = null;
    try {
      await deletion;
    } catch (error) {
      deleteError = error;
    }
    if (generation === this.generation && !this.current) {
      this.emit("idle", null, deleteError ? deleteError.code : "");
    }
  }

  closeForPageHide() {
    this.generation += 1;
    const session = this.current;
    if (!session) {
      return;
    }
    this.current = null;
    session.cancelled = true;
    session.controller.abort();
    if (session.resourceURL) {
      void deleteResource(session.resourceURL, true).catch(() => {});
    }
    this.closeLocal(session);
  }

  closeLocal(session) {
    if (session.peer) {
      session.peer.close();
    }
    if (this.video.srcObject === session.mediaStream) {
      this.video.srcObject = null;
    }
  }

  isCurrent(session) {
    return Boolean(session) && !session.cancelled && this.current === session && this.generation === session.generation;
  }

  markStreaming(session) {
    if (!this.isCurrent(session) || session.streaming) {
      return;
    }
    session.streaming = true;
    this.emit("streaming", session);
  }

  async end(session = this.current) {
    if (!this.isCurrent(session)) {
      return;
    }
    const generation = ++this.generation;
    this.current = null;
    session.cancelled = true;
    session.controller.abort();
    this.closeLocal(session);
    this.emit("ended", session, "media_ended");
    try {
      await deleteResource(session.resourceURL);
    } catch (error) {
      if (generation === this.generation && !this.current) {
        console.debug("播放器清理失败", error);
      }
    }
  }
}
