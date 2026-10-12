import { highlightOrlyscript } from "./syntax.js";

interface PackageFunction {
  parameters: Record<string, string>;
  return: string;
}

interface PackageInfo {
  name: string;
  version: number;
  functions?: Record<string, PackageFunction>;
}

interface ExampleCall {
  name: string;
  func: string;
  args: Record<string, unknown>;
  description: string;
}

interface Example {
  id: string;
  name: string;
  description: string;
  code: string;
  defaultCalls: ExampleCall[];
}

let ws: WebSocket | null = null;
let currentPov: string | null = null;
let installedPackage: PackageInfo | null = null;
let examples: Record<string, Example> = {};
let pendingRequests = new Map<number, { resolve: (val: any) => void; reject: (err: any) => void }>();
let nextReqId = 1;

// DOM Elements
const editorTextarea = document.getElementById("editor-textarea") as HTMLTextAreaElement;
const editorHighlight = document.getElementById("editor-highlight") as HTMLElement;
const lineNumbers = document.getElementById("line-numbers") as HTMLElement;
const exampleSelect = document.getElementById("example-select") as HTMLSelectElement;
const compileBtn = document.getElementById("compile-btn") as HTMLButtonElement;
const newPovBtn = document.getElementById("new-pov-btn") as HTMLButtonElement;
const statusIndicator = document.getElementById("status-indicator") as HTMLElement;
const povBadge = document.getElementById("pov-badge") as HTMLElement;
const consoleOutput = document.getElementById("console-output") as HTMLElement;
const clearConsoleBtn = document.getElementById("clear-console-btn") as HTMLButtonElement;
const packageTitle = document.getElementById("package-title") as HTMLElement;
const functionsContainer = document.getElementById("functions-container") as HTMLElement;
const quickCallsContainer = document.getElementById("quick-calls-container") as HTMLElement;
const exampleDesc = document.getElementById("example-desc") as HTMLElement;

// Initialize
async function init() {
  setupEditor();
  await loadExamples();
  connectWs();

  compileBtn.addEventListener("click", () => compileAndInstall());
  newPovBtn.addEventListener("click", () => createNewPov());
  clearConsoleBtn.addEventListener("click", () => {
    consoleOutput.innerHTML = "";
  });

  exampleSelect.addEventListener("change", () => {
    loadSelectedExample();
  });
}

function setupEditor() {
  const updateEditor = () => {
    const code = editorTextarea.value;
    editorHighlight.innerHTML = highlightOrlyscript(code);

    // Update line numbers
    const lines = code.split("\n").length;
    let nums = "";
    for (let i = 1; i <= lines; i++) {
      nums += i + "\n";
    }
    lineNumbers.textContent = nums;
  };

  editorTextarea.addEventListener("input", updateEditor);
  editorTextarea.addEventListener("scroll", () => {
    editorHighlight.scrollTop = editorTextarea.scrollTop;
    editorHighlight.scrollLeft = editorTextarea.scrollLeft;
    lineNumbers.scrollTop = editorTextarea.scrollTop;
  });

  // Tab key & Cmd+Enter
  editorTextarea.addEventListener("keydown", (e) => {
    if (e.key === "Tab") {
      e.preventDefault();
      const start = editorTextarea.selectionStart;
      const end = editorTextarea.selectionEnd;
      editorTextarea.value =
        editorTextarea.value.substring(0, start) + "  " + editorTextarea.value.substring(end);
      editorTextarea.selectionStart = editorTextarea.selectionEnd = start + 2;
      updateEditor();
    } else if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
      e.preventDefault();
      compileAndInstall();
    }
  });

  updateEditor();
}

async function loadExamples() {
  try {
    const res = await fetch("/api/examples");
    examples = await res.json();

    exampleSelect.innerHTML = "";
    for (const [id, ex] of Object.entries(examples)) {
      const opt = document.createElement("option");
      opt.value = id;
      opt.textContent = ex.name;
      exampleSelect.appendChild(opt);
    }

    loadSelectedExample();
  } catch (err) {
    logToConsole("error", `Failed to load examples: ${String(err)}`);
  }
}

function loadSelectedExample() {
  const selectedId = exampleSelect.value;
  const ex = examples[selectedId];
  if (!ex) return;

  editorTextarea.value = ex.code;
  exampleDesc.textContent = ex.description;

  // Trigger editor update
  editorTextarea.dispatchEvent(new Event("input"));

  // Update quick calls
  renderQuickCalls(ex.defaultCalls);
}

function renderQuickCalls(calls: ExampleCall[]) {
  quickCallsContainer.innerHTML = "";
  if (!calls || calls.length === 0) return;

  calls.forEach((c) => {
    const btn = document.createElement("button");
    btn.className = "quick-call-btn";
    btn.innerHTML = `<span class="quick-call-name">${c.name}</span><span class="quick-call-desc">${c.description}</span>`;
    btn.addEventListener("click", () => {
      runDirectMethod(c.func, c.args);
    });
    quickCallsContainer.appendChild(btn);
  });
}

function connectWs() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const wsUrl = `${protocol}//${location.host}/ws`;

  updateStatus("connecting", "Connecting to Orly...");

  ws = new WebSocket(wsUrl);

  ws.onopen = async () => {
    updateStatus("connected", "Connected to Orly");
    logToConsole("system", "Connected to Orly backend session");

    // Initialize session and POV
    try {
      await sendStatement("new session;");
      await createNewPov();
    } catch (e) {
      logToConsole("error", `Session init error: ${String(e)}`);
    }
  };

  ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);

      // Check if it's a response to a pending request
      if (pendingRequests.size > 0) {
        const firstKey = pendingRequests.keys().next().value;
        if (firstKey !== undefined) {
          const { resolve, reject } = pendingRequests.get(firstKey)!;
          pendingRequests.delete(firstKey);
          if (data.status === "ok") {
            resolve(data.result);
          } else {
            reject(new Error(data.result || data.status || JSON.stringify(data)));
          }
          return;
        }
      }
    } catch {
      // Non-JSON message
    }
  };

  ws.onerror = (err) => {
    updateStatus("error", "Connection error");
    logToConsole("error", `WebSocket error: ${JSON.stringify(err)}`);
  };

  ws.onclose = () => {
    updateStatus("disconnected", "Disconnected");
    logToConsole("system", "Disconnected from Orly backend. Reconnecting in 3s...");
    setTimeout(connectWs, 3000);
  };
}

function updateStatus(state: "connected" | "connecting" | "disconnected" | "error", text: string) {
  statusIndicator.className = `status-badge ${state}`;
  statusIndicator.textContent = text;
}

function sendStatement(stmt: string): Promise<any> {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return reject(new Error("WebSocket not connected"));
    }

    const reqId = nextReqId++;
    pendingRequests.set(reqId, { resolve, reject });
    ws.send(stmt);
  });
}

async function createNewPov() {
  try {
    const res = await sendStatement("new safe shared pov;");
    currentPov = res;
    povBadge.textContent = `POV: ${currentPov?.slice(0, 8)}...`;
    povBadge.title = `Full POV ID: ${currentPov}`;
    logToConsole("info", `Created new safe shared POV: ${currentPov}`);
  } catch (err) {
    logToConsole("error", `Failed to create POV: ${String(err)}`);
  }
}

async function compileAndInstall() {
  const code = editorTextarea.value.trim();
  if (!code) return;

  compileBtn.disabled = true;
  compileBtn.textContent = "Compiling...";
  const startTime = performance.now();

  try {
    // 1. Compile statement: compile "<source>";
    const compileStmt = `compile ${JSON.stringify(code)};`;
    logToConsole("stmt", compileStmt);

    const compileRes = await sendStatement(compileStmt);
    const compileDuration = (performance.now() - startTime).toFixed(1);
    logToConsole("success", `Compiled package "${compileRes.name}" v${compileRes.version} in ${compileDuration}ms`);

    // 2. Install statement: install <name>.<version>;
    const installStmt = `install ${compileRes.name}.${compileRes.version};`;
    logToConsole("stmt", installStmt);
    await sendStatement(installStmt);
    logToConsole("success", `Installed ${compileRes.name}.${compileRes.version}`);

    installedPackage = {
      name: compileRes.name,
      version: compileRes.version
    };

    // 3. Inspect functions via list packages;
    await inspectInstalledPackage(compileRes.name);
  } catch (err) {
    logToConsole("error", `Compilation/Installation error: ${String(err)}`);
  } finally {
    compileBtn.disabled = false;
    compileBtn.textContent = "Compile & Install";
  }
}

async function inspectInstalledPackage(pkgName: string) {
  try {
    const listRes = await sendStatement("list packages;");
    if (Array.isArray(listRes)) {
      const match = listRes.find((p: any) => p.name === pkgName);
      if (match) {
        installedPackage = match;
        renderPackageFunctions(match);
      }
    }
  } catch (err) {
    console.error("Failed to inspect packages:", err);
  }
}

function renderPackageFunctions(pkg: PackageInfo) {
  packageTitle.textContent = `${pkg.name} (v${pkg.version})`;
  functionsContainer.innerHTML = "";

  if (!pkg.functions || Object.keys(pkg.functions).length === 0) {
    functionsContainer.innerHTML = `<div class="empty-hint">No exported functions found in package.</div>`;
    return;
  }

  for (const [funcName, funcInfo] of Object.entries(pkg.functions)) {
    const card = document.createElement("div");
    card.className = "function-card";

    let paramsHtml = "";
    const paramNames = Object.keys(funcInfo.parameters || {});
    if (paramNames.length > 0) {
      paramsHtml = `<div class="func-params">`;
      for (const p of paramNames) {
        const pType = funcInfo.parameters[p];
        paramsHtml += `
          <div class="param-row">
            <label for="param-${funcName}-${p}">${p} <span class="param-type">${pType}</span></label>
            <input type="text" id="param-${funcName}-${p}" placeholder="value" data-type="${pType}" />
          </div>
        `;
      }
      paramsHtml += `</div>`;
    }

    card.innerHTML = `
      <div class="func-header">
        <span class="func-name">${funcName}</span>
        <span class="func-return">-> ${funcInfo.return}</span>
      </div>
      ${paramsHtml}
      <button class="call-btn" data-func="${funcName}">Run ${funcName}()</button>
    `;

    const callBtn = card.querySelector(".call-btn") as HTMLButtonElement;
    callBtn.addEventListener("click", () => {
      const args: Record<string, any> = {};
      for (const p of paramNames) {
        const input = card.querySelector(`#param-${funcName}-${p}`) as HTMLInputElement;
        const rawVal = input.value.trim();
        const pType = input.dataset.type || "";

        if (pType === "int" || pType === "real") {
          args[p] = Number(rawVal);
        } else if (pType === "bool") {
          args[p] = rawVal === "true";
        } else if (rawVal.startsWith("{") || rawVal.startsWith("[")) {
          try {
            args[p] = JSON.parse(rawVal);
          } catch {
            args[p] = rawVal;
          }
        } else {
          args[p] = rawVal;
        }
      }
      runDirectMethod(funcName, args);
    });

    functionsContainer.appendChild(card);
  }
}

async function runDirectMethod(funcName: string, args: Record<string, any>) {
  if (!installedPackage) {
    logToConsole("error", "No package installed yet. Click 'Compile & Install' first.");
    return;
  }
  if (!currentPov) {
    await createNewPov();
  }

  // Format arguments into orlyscript call: .param: val
  const argParts: string[] = [];
  for (const [k, v] of Object.entries(args)) {
    argParts.push(`.${k}: ${formatArgLiteral(v)}`);
  }
  const argsStr = argParts.join(", ");

  const stmt = `{${currentPov}} .${installedPackage.name}.${funcName}(${argsStr});`;
  logToConsole("stmt", stmt);

  const startTime = performance.now();
  try {
    const res = await sendStatement(stmt);
    const duration = (performance.now() - startTime).toFixed(1);
    logToConsole("result", JSON.stringify(res, null, 2), duration);
  } catch (err) {
    logToConsole("error", `Execution error: ${String(err)}`);
  }
}

function formatArgLiteral(v: any): string {
  if (typeof v === "number" || typeof v === "boolean") return String(v);
  if (typeof v === "string") return JSON.stringify(v);
  if (Array.isArray(v)) return `[${v.map(formatArgLiteral).join(", ")}]`;
  if (typeof v === "object" && v !== null) {
    // If tagged variant { Tag: val }
    const keys = Object.keys(v);
    if (keys.length === 1 && typeof keys[0] === "string" && /^[A-Z]/.test(keys[0])) {
      return `<| ${keys[0]}: ${formatArgLiteral(v[keys[0]])} |>`;
    }
    // Record
    const parts = keys.map((k) => `.${k}: ${formatArgLiteral(v[k])}`);
    return `<{${parts.join(", ")}}>`;
  }
  return String(v);
}

function logToConsole(type: "stmt" | "result" | "success" | "error" | "info" | "system", content: string, duration?: string) {
  const time = new Date().toLocaleTimeString();
  const entry = document.createElement("div");
  entry.className = `console-entry ${type}`;

  let header = "";
  if (type === "stmt") header = `<span class="tag stmt">STMT</span>`;
  else if (type === "result") header = `<span class="tag result">RESULT</span> ${duration ? `<span class="duration">${duration}ms</span>` : ""}`;
  else if (type === "success") header = `<span class="tag success">OK</span>`;
  else if (type === "error") header = `<span class="tag error">ERR</span>`;
  else if (type === "info") header = `<span class="tag info">INFO</span>`;
  else if (type === "system") header = `<span class="tag system">SYSTEM</span>`;

  entry.innerHTML = `
    <div class="entry-header">
      ${header}
      <span class="entry-time">${time}</span>
    </div>
    <pre class="entry-content">${escapeHtml(content)}</pre>
  `;

  consoleOutput.appendChild(entry);
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

function escapeHtml(str: string): string {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

document.addEventListener("DOMContentLoaded", init);
