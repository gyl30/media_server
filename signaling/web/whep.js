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

async function postOffer(session, offer) {
  while (!session.cancelled) {
    const response = await fetch(session.whepURL, {
      method: "POST",
      headers: {
        "Content-Type": "application/sdp",
        "X-Stream-ID": session.streamID,
      },
      body: offer,
    });
    if (response.status === 409) {
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
    const contentType = response.headers.get("Content-Type") || "";
    const answer = await response.text();
    if (!location || !contentType.toLowerCase().startsWith("application/sdp") || !answer) {
      throw new WHEPError(response.status, "invalid_whep_response");
    }
    return { answer, resourceURL: new URL(location, session.whepURL).toString() };
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
      streamID: session ? session.streamID : "",
      error,
    });
  }

  async start(target, label) {
    const generation = ++this.generation;
    try {
      await this.stopCurrent(generation);
    } catch (error) {
      if (generation === this.generation) {
        throw error;
      }
      return null;
    }
    if (generation !== this.generation) {
      return null;
    }
    const session = {
      cancelled: false,
      controller: new AbortController(),
      generation,
      label,
      mediaStream: new MediaStream(),
      peer: null,
      resourceURL: "",
      streaming: false,
      streamID: "",
      whepURL: "",
    };
    this.current = session;
    this.emit("allocating", session);
    try {
      const allocation = await api.startPreview(target, session.controller.signal);
      if (!this.isCurrent(session)) {
        return null;
      }
      session.streamID = allocation.stream_id;
      session.whepURL = allocation.whep_url;
      session.peer = new RTCPeerConnection();
      session.peer.addTransceiver("video", { direction: "recvonly" });
      session.peer.addTransceiver("audio", { direction: "recvonly" });
      session.peer.addEventListener("track", (event) => {
        if (![...session.mediaStream.getTracks()].some((track) => track.id === event.track.id)) {
          session.mediaStream.addTrack(event.track);
        }
        if (this.current === session) {
          this.video.srcObject = session.mediaStream;
          void this.video.play().catch(() => {});
        }
      });
      session.peer.addEventListener("connectionstatechange", () => {
        if (this.current !== session) {
          return;
        }
        if (session.peer.connectionState === "connected") {
          this.markStreaming(session);
        } else if (session.peer.connectionState === "failed") {
          void this.fail(session, "webrtc_connection_failed");
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
      const result = await postOffer(session, session.peer.localDescription.sdp);
      session.resourceURL = result.resourceURL;
      if (!this.isCurrent(session)) {
        await deleteResource(session.resourceURL);
        this.closeLocal(session);
        return null;
      }
      await session.peer.setRemoteDescription({ type: "answer", sdp: result.answer });
      if (!this.isCurrent(session)) {
        await deleteResource(session.resourceURL);
        this.closeLocal(session);
        return null;
      }
      if (session.peer.connectionState === "connected") {
        this.markStreaming(session);
      }
      return { streamID: session.streamID, whepURL: session.whepURL };
    } catch (error) {
      if (session.resourceURL) {
        try {
          await deleteResource(session.resourceURL);
        } catch {
        }
      }
      this.closeLocal(session);
      if (this.current === session) {
        this.current = null;
        this.emit("idle", null);
      }
      if (session.cancelled || error.name === "AbortError") {
        return null;
      }
      throw error;
    }
  }

  async stop() {
    const generation = ++this.generation;
    return this.stopCurrent(generation);
  }

  async stopCurrent(generation) {
    const session = this.current;
    if (!session) {
      return;
    }
    this.current = null;
    session.cancelled = true;
    session.controller.abort();
    this.emit("stopping", session);
    let deleteError = null;
    try {
      await deleteResource(session.resourceURL);
    } catch (error) {
      deleteError = error;
    } finally {
      this.closeLocal(session);
    }
    if (generation === this.generation && !this.current) {
      this.emit("idle", null, deleteError ? deleteError.code : "");
    }
    if (deleteError && generation === this.generation) {
      throw deleteError;
    }
  }

  stopForPageHide() {
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

  stopIfStream(streamID) {
    if (this.current && this.current.streamID === streamID) {
      return this.stop();
    }
    return Promise.resolve();
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
    return !session.cancelled && this.current === session && this.generation === session.generation;
  }

  markStreaming(session) {
    if (!this.isCurrent(session) || session.streaming) {
      return;
    }
    session.streaming = true;
    this.emit("streaming", session);
  }

  async fail(session, code) {
    if (!this.isCurrent(session)) {
      return;
    }
    const generation = ++this.generation;
    this.current = null;
    session.cancelled = true;
    session.controller.abort();
    this.closeLocal(session);
    this.emit("failed", session, code);
    try {
      await deleteResource(session.resourceURL);
    } catch (error) {
      if (generation === this.generation && !this.current) {
        this.emit("failed", session, error.code || code);
      }
    }
  }
}
