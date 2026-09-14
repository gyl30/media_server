export class APIError extends Error {
  constructor(status, code) {
    super(code || `http_${status}`);
    this.name = "APIError";
    this.status = status;
    this.code = code || `http_${status}`;
  }
}

async function request(path, options = {}) {
  const init = {
    method: options.method || "GET",
    headers: { Accept: "application/json" },
    signal: options.signal,
  };
  if (init.method === "GET") {
    init.cache = "no-store";
  }
  if (options.body !== undefined) {
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(options.body);
  }
  const response = await fetch(path, init);
  const contentType = response.headers.get("Content-Type") || "";
  let payload = null;
  if (response.status !== 204) {
    const text = await response.text();
    if (text && contentType.toLowerCase().includes("application/json")) {
      try {
        payload = JSON.parse(text);
      } catch {
        payload = null;
      }
    }
  }
  if (!response.ok) {
    throw new APIError(response.status, payload && typeof payload.error === "string" ? payload.error : "");
  }
  return payload;
}

function segment(value) {
  return encodeURIComponent(value);
}

export const api = {
  mediaServers: (signal) => request("/api/media-servers", { signal }),
  sources: (signal) => request("/api/sources", { signal }),
  runtimes: (signal) => request("/api/runtimes", { signal }),
  devices: (signal) => request("/api/devices", { signal }),
  channels: (deviceID, signal) => request(`/api/devices/${segment(deviceID)}/channels`, { signal }),
  createSource: (body) => request("/api/sources", { method: "POST", body }),
  patchSource: (sourceID, body) => request(`/api/sources/${segment(sourceID)}`, { method: "PATCH", body }),
  deleteSource: (sourceID) => request(`/api/sources/${segment(sourceID)}`, { method: "DELETE" }),
  startSource: (sourceID) => request(`/api/sources/${segment(sourceID)}/start`, { method: "POST" }),
  stopSource: (sourceID) => request(`/api/sources/${segment(sourceID)}/stop`, { method: "POST" }),
  startChannel: (deviceID, channelID) => request(
    `/api/devices/${segment(deviceID)}/channels/${segment(channelID)}/start`,
    { method: "POST" },
  ),
  stopChannel: (deviceID, channelID, streamID) => request(
    `/api/devices/${segment(deviceID)}/channels/${segment(channelID)}/stop`,
    { method: "POST", body: { stream_id: streamID } },
  ),
  startPreview: (body, signal) => request("/api/preview/start", { method: "POST", body, signal }),
};
