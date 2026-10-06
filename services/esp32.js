const DEFAULT_BASE_URL = "http://192.168.4.1";
let baseUrl = DEFAULT_BASE_URL;

export function normalizeBaseUrl(value) {
  let url = (value || "").trim();
  if (!url) return DEFAULT_BASE_URL;
  if (!/^https?:\/\//i.test(url)) url = `http://${url}`;
  return url.replace(/\/+$/, "");
}
export function setBaseUrl(value) { baseUrl = normalizeBaseUrl(value); return baseUrl; }
export function getBaseUrl() { return baseUrl; }

async function request(path, options = {}, timeoutMs = 3500) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`${baseUrl}${path}`, {
      ...options,
      signal: controller.signal,
      headers: { Accept: "application/json", ...(options.headers || {}) },
    });
    const text = await response.text();
    let data;
    try { data = JSON.parse(text); } catch { data = { raw: text }; }
    if (!response.ok) throw new Error(data?.error || `HTTP ${response.status}`);
    return data;
  } finally { clearTimeout(timer); }
}

export const getStatus = () => request("/status");
export const armSystem = () => request("/arm", { method: "POST" });
export const disarmSystem = () => request("/disarm", { method: "POST" });
export const clearIntrusion = () => request("/clear", { method: "POST" });
export const getStreamUrl = () => `${baseUrl}:81/stream`;
export const getCaptureUrl = () => `${baseUrl}/capture`;
export const syncTime = (epochSeconds = Math.floor(Date.now() / 1000)) => request(`/time?epoch=${epochSeconds}`);
