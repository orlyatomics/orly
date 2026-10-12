import * as http from "node:http";
import * as fs from "node:fs";
import * as path from "node:path";
import { WebSocketServer, WebSocket } from "ws";
import { EXAMPLES } from "./examples.js";

const PORT = parseInt(process.env.PORT || "3000", 10);
const HOST = process.env.HOST || "0.0.0.0";
const ORLY_WS_URL = process.env.ORLY_WS_URL || "ws://127.0.0.1:8082/";
const ORLY_AUTH_TOKEN = process.env.ORLY_AUTH_TOKEN || "";
const SESSION_TIMEOUT_MS = parseInt(process.env.SESSION_TIMEOUT_MS || "900000", 10); // 15 minutes default

// Resolve static public directory
const PUBLIC_DIR = path.resolve(process.cwd(), "public");

const MIME_TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
};

// HTTP Server
const server = http.createServer((req, res) => {
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);

  // API: Preloaded examples
  if (url.pathname === "/api/examples") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(EXAMPLES));
    return;
  }

  // API: Status
  if (url.pathname === "/api/status") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({
      status: "ok",
      orlyUrl: ORLY_WS_URL,
      authConfigured: Boolean(ORLY_AUTH_TOKEN),
      sessionTimeoutMs: SESSION_TIMEOUT_MS
    }));
    return;
  }

  // Static file serving
  let reqPath = url.pathname;
  if (reqPath === "/" || reqPath === "") {
    reqPath = "/index.html";
  }

  const safePath = path.normalize(reqPath).replace(/^(\.\.[\/\\])+/, "");
  const filePath = path.join(PUBLIC_DIR, safePath);

  if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
    const ext = path.extname(filePath).toLowerCase();
    const contentType = MIME_TYPES[ext] || "application/octet-stream";
    res.writeHead(200, { "Content-Type": contentType });
    fs.createReadStream(filePath).pipe(res);
  } else {
    // Fallback to index.html for SPA
    const indexPath = path.join(PUBLIC_DIR, "index.html");
    if (fs.existsSync(indexPath)) {
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
      fs.createReadStream(indexPath).pipe(res);
    } else {
      res.writeHead(404, { "Content-Type": "text/plain" });
      res.end("Not Found");
    }
  }
});

// WebSocket Server Proxy to orlyi
const wss = new WebSocketServer({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  if (url.pathname === "/ws") {
    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit("connection", ws, req);
    });
  } else {
    socket.destroy();
  }
});

wss.on("connection", (clientWs: WebSocket) => {
  const sessionId = "s-" + Math.random().toString(36).slice(2, 10);
  console.log(`[Playground] Client connected, session=${sessionId}`);

  let orlyWs: WebSocket | null = null;
  let inactivityTimer: NodeJS.Timeout | null = null;
  const pendingClientMessages: string[] = [];
  let orlyReady = false;

  const resetInactivity = () => {
    if (inactivityTimer) clearTimeout(inactivityTimer);
    inactivityTimer = setTimeout(() => {
      console.log(`[Playground] Session ${sessionId} timed out from inactivity`);
      if (orlyWs && orlyWs.readyState === WebSocket.OPEN) {
        orlyWs.send("exit;");
        orlyWs.close();
      }
      if (clientWs.readyState === WebSocket.OPEN) {
        clientWs.send(JSON.stringify({
          status: "timeout",
          error: "Session timed out after 15 minutes of inactivity."
        }));
        clientWs.close();
      }
    }, SESSION_TIMEOUT_MS);
  };

  resetInactivity();

  // Connect to upstream orlyi
  try {
    orlyWs = new WebSocket(ORLY_WS_URL);
  } catch (err) {
    clientWs.send(JSON.stringify({
      status: "error",
      error: `Could not connect to Orly backend at ${ORLY_WS_URL}: ${String(err)}`
    }));
    clientWs.close();
    return;
  }

  orlyWs.on("open", () => {
    console.log(`[Playground] Upstream orlyi connected for session ${sessionId}`);

    // If shared secret (#710) is configured, authenticate as the first message
    if (ORLY_AUTH_TOKEN) {
      orlyWs?.send(JSON.stringify({ auth: ORLY_AUTH_TOKEN }));
    } else {
      orlyReady = true;
      flushPending();
    }
  });

  orlyWs.on("message", (data: Buffer | string) => {
    resetInactivity();
    const str = data.toString();

    // If we just authenticated, check auth status
    if (ORLY_AUTH_TOKEN && !orlyReady) {
      try {
        const parsed = JSON.parse(str);
        if (parsed.status === "ok") {
          orlyReady = true;
          flushPending();
          return;
        } else {
          clientWs.send(JSON.stringify({
            status: "error",
            error: `Upstream Orly authentication failed: ${parsed.status}`
          }));
          clientWs.close();
          return;
        }
      } catch {
        // Fall through
      }
    }

    if (clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(str);
    }
  });

  orlyWs.on("error", (err) => {
    console.error(`[Playground] Upstream error for session ${sessionId}:`, err);
    if (clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(JSON.stringify({
        status: "error",
        error: `Upstream Orly error: ${err.message}`
      }));
    }
  });

  orlyWs.on("close", (code, reason) => {
    console.log(`[Playground] Upstream closed for session ${sessionId}: code=${code} reason=${reason}`);
    if (clientWs.readyState === WebSocket.OPEN) {
      clientWs.close(code, reason);
    }
  });

  const flushPending = () => {
    while (pendingClientMessages.length > 0 && orlyWs && orlyWs.readyState === WebSocket.OPEN) {
      const msg = pendingClientMessages.shift()!;
      orlyWs.send(msg);
    }
  };

  clientWs.on("message", (data) => {
    resetInactivity();
    const msg = data.toString();
    if (orlyReady && orlyWs && orlyWs.readyState === WebSocket.OPEN) {
      orlyWs.send(msg);
    } else {
      pendingClientMessages.push(msg);
    }
  });

  clientWs.on("close", () => {
    console.log(`[Playground] Client closed, session=${sessionId}`);
    if (inactivityTimer) clearTimeout(inactivityTimer);
    if (orlyWs && orlyWs.readyState === WebSocket.OPEN) {
      orlyWs.send("exit;");
      orlyWs.close();
    }
  });
});

server.listen(PORT, HOST, () => {
  console.log(`Orlyscript Playground listening on http://${HOST}:${PORT}`);
  console.log(`Proxying to Orly backend at ${ORLY_WS_URL}`);
});
