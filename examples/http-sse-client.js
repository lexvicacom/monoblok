// Tiny dependency-free HTTP/SSE client for monoblok's HTTP adapter.
// Works in modern browsers and Node 18+ where fetch and ReadableStream exist.

function subjectPath(subject) {
  if (typeof subject !== "string" || subject.length === 0) {
    throw new TypeError("subject must be a non-empty string");
  }
  return subject.split(".").map(encodeURIComponent).join("/");
}

function basicBase64(text) {
  if (typeof btoa === "function") {
    return btoa(text);
  }
  if (typeof Buffer !== "undefined") {
    return Buffer.from(text, "utf8").toString("base64");
  }
  throw new Error("no base64 encoder available");
}

function authHeader(auth) {
  if (!auth) return null;
  if (auth.token) return `Bearer ${auth.token}`;
  if (auth.user != null && auth.pass != null) {
    return `Basic ${basicBase64(`${auth.user}:${auth.pass}`)}`;
  }
  return null;
}

async function* sseEvents(stream) {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let text = "";
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      text += decoder.decode(value, { stream: true });
      for (;;) {
        const lf = text.indexOf("\n\n");
        const crlf = text.indexOf("\r\n\r\n");
        const idx = lf === -1 ? crlf : crlf === -1 ? lf : Math.min(lf, crlf);
        if (idx === -1) break;
        const sepLen = text.startsWith("\r\n\r\n", idx) ? 4 : 2;
        const raw = text.slice(0, idx);
        text = text.slice(idx + sepLen);
        let event = "message";
        const data = [];
        for (const line of raw.replaceAll("\r\n", "\n").split("\n")) {
          if (line.startsWith(":")) continue;
          if (line.startsWith("event:")) event = line.slice(6).trimStart();
          if (line.startsWith("data:")) data.push(line.slice(5).trimStart());
        }
        if (data.length !== 0) {
          yield { event, data: data.join("\n") };
        }
      }
    }
  } finally {
    reader.releaseLock();
  }
}

class MonoblokHttpClient {
  constructor({ baseUrl = "http://127.0.0.1:8080", token, user, pass, fetchImpl = globalThis.fetch } = {}) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.auth = { token, user, pass };
    this.fetch = fetchImpl;
    if (typeof this.fetch !== "function") {
      throw new Error("fetch is not available");
    }
  }

  headers(extra = {}) {
    const headers = { ...extra };
    const auth = authHeader(this.auth);
    if (auth) headers.Authorization = auth;
    return headers;
  }

  async publish(subject, payload, { contentType = "text/plain" } = {}) {
    if (typeof payload !== "string") {
      throw new TypeError("monoblok HTTP publish expects a text string payload");
    }
    const res = await this.fetch(`${this.baseUrl}/pub/${subjectPath(subject)}`, {
      method: "POST",
      headers: this.headers({ "Content-Type": contentType }),
      body: payload,
    });
    if (!res.ok) {
      throw new Error(`monoblok publish failed: HTTP ${res.status} ${res.statusText}`);
    }
  }

  async *messages(subject, { signal } = {}) {
    const res = await this.fetch(`${this.baseUrl}/sub/${subjectPath(subject)}`, {
      method: "GET",
      headers: this.headers(),
      signal,
    });
    if (!res.ok || !res.body) {
      throw new Error(`monoblok subscribe failed: HTTP ${res.status} ${res.statusText}`);
    }
    for await (const event of sseEvents(res.body)) {
      if (event.event !== "msg") continue;
      yield JSON.parse(event.data);
    }
  }

  subscribe(subject, onMessage, { onError } = {}) {
    const controller = new AbortController();
    (async () => {
      try {
        for await (const msg of this.messages(subject, { signal: controller.signal })) {
          onMessage(msg);
        }
      } catch (err) {
        if (!controller.signal.aborted) {
          if (onError) onError(err);
          else if (typeof console !== "undefined" && console.error) console.error(err);
        }
      }
    })();
    return () => controller.abort();
  }
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = { MonoblokHttpClient };
} else {
  globalThis.MonoblokHttpClient = MonoblokHttpClient;
}
