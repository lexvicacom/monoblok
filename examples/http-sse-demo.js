#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const http = require("node:http");
const https = require("node:https");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { URL } = require("node:url");

const root = __dirname;
const repoRoot = path.resolve(root, "..");
const hopHeaders = new Set([
  "connection",
  "keep-alive",
  "proxy-authenticate",
  "proxy-authorization",
  "te",
  "trailer",
  "transfer-encoding",
  "upgrade",
]);

function usage() {
  return [
    "usage: node examples/http-sse-demo.js [--host HOST] [--port PORT] [--monoblok URL] [--open] [--start-monoblok]",
    "",
    "Serves examples/http-sse-client.html and proxies /sub and /pub to monoblok.",
    "Without --start-monoblok, monoblok must already be running with --http-port.",
    "",
    "Defaults:",
    "  --host 127.0.0.1",
    "  --port 8090",
    "  --monoblok http://127.0.0.1:8080",
    "  --monoblok-bin build/monoblok",
    "  --patchbay patchbay.edn",
    "  --nats-port 14222",
  ].join("\n");
}

function parsePort(value, label) {
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error(`${label} must be a TCP port`);
  }
  return port;
}

function parseArgs(argv) {
  const opts = {
    host: process.env.HOST || "127.0.0.1",
    port: parsePort(process.env.PORT || "8090", "PORT"),
    monoblok: process.env.MONOBLOK_HTTP || "http://127.0.0.1:8080",
    monoblokBin: process.env.MONOBLOK_BIN || path.join(repoRoot, "build", "monoblok"),
    patchbay: process.env.PATCHBAY || path.join(repoRoot, "patchbay.edn"),
    natsPort: parsePort(process.env.NATS_PORT || "14222", "NATS_PORT"),
    open: process.env.OPEN_BROWSER === "1",
    startMonoblok: process.env.START_MONOBLOK === "1",
  };
  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      console.log(usage());
      process.exit(0);
    }
    if (arg === "--host" && i + 1 < argv.length) {
      opts.host = argv[++i];
    } else if (arg === "--port" && i + 1 < argv.length) {
      opts.port = parsePort(argv[++i], "--port");
    } else if (arg === "--monoblok" && i + 1 < argv.length) {
      opts.monoblok = argv[++i];
    } else if (arg === "--monoblok-bin" && i + 1 < argv.length) {
      opts.monoblokBin = path.resolve(argv[++i]);
    } else if (arg === "--patchbay" && i + 1 < argv.length) {
      opts.patchbay = path.resolve(argv[++i]);
    } else if (arg === "--nats-port" && i + 1 < argv.length) {
      opts.natsPort = parsePort(argv[++i], "--nats-port");
    } else if (arg === "--open") {
      opts.open = true;
    } else if (arg === "--start-monoblok") {
      opts.startMonoblok = true;
    } else {
      throw new Error(`unknown or incomplete argument: ${arg}`);
    }
  }
  const upstream = new URL(opts.monoblok);
  if (upstream.protocol !== "http:" && upstream.protocol !== "https:") {
    throw new Error("--monoblok must be an http or https URL");
  }
  upstream.pathname = upstream.pathname.replace(/\/+$/, "");
  upstream.search = "";
  upstream.hash = "";
  return { ...opts, upstream };
}

function upstreamPort(upstream) {
  if (upstream.port.length !== 0) {
    return parsePort(upstream.port, "--monoblok port");
  }
  return upstream.protocol === "https:" ? 443 : 80;
}

function browserUrl(host, port) {
  let displayHost = host;
  if (displayHost === "0.0.0.0" || displayHost === "::") {
    displayHost = "127.0.0.1";
  }
  if (displayHost.includes(":") && !displayHost.startsWith("[")) {
    displayHost = `[${displayHost}]`;
  }
  return `http://${displayHost}:${port}/`;
}

function openBrowser(url) {
  let command = "xdg-open";
  let args = [url];
  if (process.platform === "darwin") {
    command = "open";
  } else if (process.platform === "win32") {
    command = "cmd";
    args = ["/c", "start", "", url];
  }
  const child = spawn(command, args, {
    detached: true,
    stdio: "ignore",
  });
  child.on("error", (err) => {
    console.error(`could not open browser: ${err.message}`);
  });
  child.unref();
}

function startMonoblok(opts) {
  if (opts.upstream.protocol !== "http:") {
    throw new Error("--start-monoblok only supports an http:// --monoblok URL");
  }
  if (opts.upstream.pathname !== "" && opts.upstream.pathname !== "/") {
    throw new Error("--start-monoblok does not support a path prefix in --monoblok");
  }
  if (!fs.existsSync(opts.monoblokBin)) {
    throw new Error(`${opts.monoblokBin} does not exist; build monoblok first or pass --monoblok-bin`);
  }
  const httpPort = upstreamPort(opts.upstream);
  const args = [
    "--host", "127.0.0.1",
    "--port", String(opts.natsPort),
    "--patchbay", opts.patchbay,
    "--http-host", opts.upstream.hostname,
    "--http-port", String(httpPort),
  ];
  const child = spawn(opts.monoblokBin, args, {
    cwd: repoRoot,
    stdio: ["ignore", "inherit", "inherit"],
  });
  console.log(`started monoblok: ${opts.monoblokBin} ${args.join(" ")}`);
  return child;
}

function waitForUpstream(upstream, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  const transport = upstream.protocol === "https:" ? https : http;
  const tryOnce = (resolve, reject) => {
    const req = transport.request({
      protocol: upstream.protocol,
      hostname: upstream.hostname,
      port: upstreamPort(upstream),
      method: "GET",
      path: "/",
      timeout: 250,
    }, (res) => {
      res.resume();
      res.on("end", resolve);
    });
    req.on("timeout", () => req.destroy(new Error("timeout")));
    req.on("error", (err) => {
      if (Date.now() >= deadline) {
        reject(err);
        return;
      }
      setTimeout(() => tryOnce(resolve, reject), 100);
    });
    req.end();
  };
  return new Promise(tryOnce);
}

function sendText(res, status, text) {
  const body = Buffer.from(text, "utf8");
  res.writeHead(status, {
    "Content-Type": "text/plain; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-cache",
  });
  res.end(body);
}

function serveFile(res, fileName, contentType) {
  fs.readFile(path.join(root, fileName), (err, body) => {
    if (err) {
      sendText(res, 500, `${fileName} not found\n`);
      return;
    }
    res.writeHead(200, {
      "Content-Type": contentType,
      "Content-Length": body.length,
      "Cache-Control": "no-cache",
    });
    res.end(body);
  });
}

function cleanHeaders(headers) {
  const out = {};
  for (const [name, value] of Object.entries(headers)) {
    const lower = name.toLowerCase();
    if (hopHeaders.has(lower)) {
      continue;
    }
    out[name] = value;
  }
  return out;
}

function proxyToMonoblok(req, res, upstreamBase, reqUrl) {
  const target = new URL(upstreamBase.href);
  const basePath = target.pathname.replace(/\/+$/, "");
  target.pathname = `${basePath}${reqUrl.pathname}`;
  target.search = reqUrl.search;
  const transport = target.protocol === "https:" ? https : http;
  const headers = cleanHeaders(req.headers);
  headers.host = target.host;

  let upstreamRes = null;
  const upstreamReq = transport.request({
    protocol: target.protocol,
    hostname: target.hostname,
    port: target.port,
    method: req.method,
    path: `${target.pathname}${target.search}`,
    headers,
  }, (proxyRes) => {
    upstreamRes = proxyRes;
    res.writeHead(proxyRes.statusCode || 502, proxyRes.statusMessage, cleanHeaders(proxyRes.headers));
    res.flushHeaders();
    proxyRes.pipe(res);
  });

  upstreamReq.on("error", (err) => {
    if (res.headersSent) {
      res.destroy(err);
    } else {
      sendText(res, 502, `monoblok proxy failed: ${err.message}\n`);
    }
  });

  req.on("aborted", () => upstreamReq.destroy());
  res.on("close", () => {
    upstreamReq.destroy();
    if (upstreamRes) {
      upstreamRes.destroy();
    }
  });

  req.pipe(upstreamReq);
}

function route(req, res, upstream) {
  let reqUrl;
  try {
    reqUrl = new URL(req.url || "/", "http://local");
  } catch (err) {
    sendText(res, 400, "bad request target\n");
    return;
  }
  const pathname = reqUrl.pathname;

  if (req.method === "GET" && (pathname === "/" || pathname === "/http-sse-client.html")) {
    serveFile(res, "http-sse-client.html", "text/html; charset=utf-8");
    return;
  }
  if (req.method === "GET" && pathname === "/http-sse-client.js") {
    serveFile(res, "http-sse-client.js", "application/javascript; charset=utf-8");
    return;
  }
  if (pathname.startsWith("/sub/")) {
    if (req.method !== "GET") {
      sendText(res, 405, "use GET for /sub\n");
      return;
    }
    proxyToMonoblok(req, res, upstream, reqUrl);
    return;
  }
  if (pathname.startsWith("/pub/")) {
    if (req.method !== "POST") {
      sendText(res, 405, "use POST for /pub\n");
      return;
    }
    proxyToMonoblok(req, res, upstream, reqUrl);
    return;
  }

  sendText(res, 404, "not found\n");
}

function main() {
  let opts;
  try {
    opts = parseArgs(process.argv);
  } catch (err) {
    console.error(err.message);
    console.error("");
    console.error(usage());
    process.exit(2);
  }

  let monoblokChild = null;
  if (opts.startMonoblok) {
    try {
      monoblokChild = startMonoblok(opts);
    } catch (err) {
      console.error(err.message);
      process.exit(2);
    }
  }

  const server = http.createServer((req, res) => route(req, res, opts.upstream));
  server.on("error", (err) => {
    console.error(`demo server failed: ${err.message}`);
    if (monoblokChild && !monoblokChild.killed) {
      monoblokChild.kill("SIGTERM");
    }
    process.exit(1);
  });
  server.on("clientError", (_err, socket) => {
    socket.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  });
  const stop = (signal) => {
    server.close();
    if (monoblokChild && !monoblokChild.killed) {
      monoblokChild.kill("SIGTERM");
    }
    process.exit(signal === "SIGINT" ? 130 : 143);
  };
  process.on("SIGINT", () => stop("SIGINT"));
  process.on("SIGTERM", () => stop("SIGTERM"));
  if (monoblokChild) {
    monoblokChild.on("exit", (code, signal) => {
      console.error(`monoblok exited${signal ? ` from ${signal}` : ` with ${code}`}`);
    });
  }

  server.listen(opts.port, opts.host, async () => {
    const url = browserUrl(opts.host, opts.port);
    console.log(`monoblok HTTP/SSE demo: ${url}`);
    console.log(`proxying /sub and /pub to ${opts.upstream.href.replace(/\/$/, "")}`);
    if (opts.startMonoblok) {
      try {
        await waitForUpstream(opts.upstream, 4000);
      } catch (err) {
        console.error(`monoblok HTTP listener was not reachable yet: ${err.message}`);
      }
    }
    if (opts.open) {
      openBrowser(url);
    }
  });
}

main();
