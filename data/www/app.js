/* app.js — ESP32-S3 SWD Programozó web-UI logikája.
 *
 * Tiszta vanilla JS, nincs build-lépés, nincs külső függőség.
 * REST végpontok (lásd components/web_ui/web_ui.c):
 *   GET    /api/files?dir=fw|cfg      -> [{name,size},...]
 *   POST   /api/upload?path=/fw/x.bin -> body = nyers fájl (streamelt)
 *   GET    /api/download?path=...     -> fájl letöltés
 *   DELETE /api/file?path=...         -> törlés
 *   POST   /api/program?file=/fw/x.bin-> 202 {started} | 409 {busy}
 *   POST   /api/cfg/pull?name=x.cfg   -> cél -> ESP másolás
 *   POST   /api/cfg/push?name=x.cfg   -> ESP -> cél másolás
 * WebSocket /ws:
 *   {type:"state", ...}  ~1mp-enként cél-állapot
 *   {type:"prog",  ...}  flash közben haladás (phase, percent, target_name, ...)
 */

"use strict";

/* ---- Apró segédek ---- */
const $ = (sel) => document.querySelector(sel);

function fmtBytes(n) {
  if (n == null) return "?";
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " kB";
  return (n / (1024 * 1024)).toFixed(2) + " MB";
}

function fmtUptime(s) {
  s = Number(s) || 0;
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h) return `${h} ó ${m} p ${sec} mp`;
  if (m) return `${m} p ${sec} mp`;
  return `${sec} mp`;
}

/* ---- Token-kezelés (terv 12.2) ---- */
const TOKEN_KEY = "swdprog_token";

function getToken() {
  try { return localStorage.getItem(TOKEN_KEY) || ""; }
  catch (e) { return ""; }
}

function setToken(t) {
  try {
    if (t) localStorage.setItem(TOKEN_KEY, t);
    else localStorage.removeItem(TOKEN_KEY);
  } catch (e) { /* localStorage nem elérhető */ }
}

/* Token-fejléc hozzáadása a fetch opciókhoz (ha van token). */
function withAuth(opts) {
  opts = opts || {};
  const t = getToken();
  if (t) {
    opts.headers = Object.assign({}, opts.headers, { "X-Auth-Token": t });
  }
  return opts;
}

/* Token-aware fetch: minden REST hívás ezen megy át. 401-nél egyértelmű üzenet. */
async function apiFetch(url, opts) {
  const r = await fetch(url, withAuth(opts));
  if (r.status === 401) {
    toast("Hibás vagy hiányzó token", "err");
  }
  return r;
}

let toastTimer = null;
function toast(msg, kind) {
  const t = $("#toast");
  t.textContent = msg;
  t.className = "toast" + (kind ? " " + kind : "");
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

/* ===================================================================== */
/* Fájllisták                                                            */
/* ===================================================================== */

async function loadFiles(dir) {
  const listEl = $("#" + dir + "List");
  try {
    const r = await apiFetch("/api/files?dir=" + dir);
    if (!r.ok) throw new Error("HTTP " + r.status);
    const items = await r.json();
    renderFiles(dir, listEl, items);
  } catch (e) {
    listEl.innerHTML = '<li class="empty">Hiba a lista betöltésekor</li>';
  }
}

function renderFiles(dir, listEl, items) {
  if (!Array.isArray(items) || items.length === 0) {
    listEl.innerHTML = '<li class="empty">Nincs fájl</li>';
    return;
  }
  listEl.innerHTML = "";
  for (const f of items) {
    const path = "/" + dir + "/" + f.name;      // pl. /fw/app.bin
    const li = document.createElement("li");

    const name = document.createElement("span");
    name.className = "fname";
    name.textContent = f.name;

    const size = document.createElement("span");
    size.className = "fsize";
    size.textContent = fmtBytes(f.size);

    const actions = document.createElement("div");
    actions.className = "factions";

    // Letöltés (sima link a /api/download-ra)
    const dl = document.createElement("a");
    dl.className = "btn ghost small";
    dl.textContent = "Letöltés";
    // Letöltés sima link -> nem tud fejlécet küldeni, ezért a token query-be megy
    dl.href = "/api/download?path=" + encodeURIComponent(path) +
              (getToken() ? "&token=" + encodeURIComponent(getToken()) : "");
    actions.appendChild(dl);

    if (dir === "fw") {
      // Flash gomb -> POST /api/program
      const flash = document.createElement("button");
      flash.className = "btn flash small";
      flash.textContent = "Flash";
      flash.onclick = () => programFile(path, flash);
      actions.appendChild(flash);
    } else {
      // cfg push / pull a soros hídon át (név alapján)
      const push = document.createElement("button");
      push.className = "btn small";
      push.textContent = "Push →cél";
      push.onclick = () => cfgOp("push", f.name, push);
      actions.appendChild(push);

      const pull = document.createElement("button");
      pull.className = "btn ghost small";
      pull.textContent = "Pull ←cél";
      pull.onclick = () => cfgOp("pull", f.name, pull);
      actions.appendChild(pull);
    }

    // Törlés -> DELETE /api/file
    const del = document.createElement("button");
    del.className = "btn danger small";
    del.textContent = "Törlés";
    del.onclick = () => deleteFile(dir, path, del);
    actions.appendChild(del);

    li.appendChild(name);
    li.appendChild(size);
    li.appendChild(actions);
    listEl.appendChild(li);
  }
}

/* ===================================================================== */
/* Műveletek                                                            */
/* ===================================================================== */

async function deleteFile(dir, path, btn) {
  if (!confirm("Biztosan törlöd ezt: " + path + " ?")) return;
  btn.disabled = true;
  try {
    const r = await apiFetch("/api/file?path=" + encodeURIComponent(path), { method: "DELETE" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    toast("Törölve: " + path, "ok");
    loadFiles(dir);
  } catch (e) {
    toast("Törlés sikertelen: " + path, "err");
    btn.disabled = false;
  }
}

async function programFile(path, btn) {
  if (!confirm("Flashelés indítása: " + path + " ?")) return;
  btn.disabled = true;
  try {
    const r = await apiFetch("/api/program?file=" + encodeURIComponent(path), { method: "POST" });
    if (r.status === 401) {
      return;   // a 401 üzenetet az apiFetch már kiírta
    } else if (r.status === 409) {
      toast("Egy flash folyamat már fut (busy).", "err");
    } else if (r.status === 202 || r.ok) {
      toast("Flash elindítva: " + path + " — haladás a folyamatsávon.", "ok");
      showProg();
    } else {
      throw new Error("HTTP " + r.status);
    }
  } catch (e) {
    toast("Flash indítás sikertelen.", "err");
  } finally {
    setTimeout(() => { btn.disabled = false; }, 1500);
  }
}

async function cfgOp(op, name, btn) {
  btn.disabled = true;
  try {
    const r = await apiFetch("/api/cfg/" + op + "?name=" + encodeURIComponent(name), { method: "POST" });
    if (r.status === 401) return;   // a 401 üzenetet az apiFetch már kiírta
    if (!r.ok) throw new Error("HTTP " + r.status);
    toast((op === "push" ? "Push →cél" : "Pull ←cél") + " kész: " + name, "ok");
    if (op === "pull") loadFiles("cfg");   // pull után frissül a tartalom
  } catch (e) {
    toast("cfg " + op + " sikertelen: " + name, "err");
  } finally {
    btn.disabled = false;
  }
}

/* Feltöltés: a nyers File-t streameljük a body-ban a /api/upload-ra. */
function setupUploader(dir, fileInputId, btnId, statusId) {
  const input = $("#" + fileInputId);
  const btn = $("#" + btnId);
  const status = $("#" + statusId);

  btn.onclick = async () => {
    const file = input.files && input.files[0];
    if (!file) { status.className = "upstatus err"; status.textContent = "Válassz fájlt!"; return; }

    const path = "/" + dir + "/" + file.name;
    btn.disabled = true;
    status.className = "upstatus";
    status.textContent = "Feltöltés… (" + fmtBytes(file.size) + ")";

    try {
      // A body közvetlenül a File objektum -> a böngésző streameli.
      const r = await apiFetch("/api/upload?path=" + encodeURIComponent(path), {
        method: "POST",
        body: file,
        headers: { "Content-Type": "application/octet-stream" },
      });
      if (!r.ok) throw new Error("HTTP " + r.status);
      status.className = "upstatus ok";
      status.textContent = "Feltöltve: " + file.name;
      input.value = "";
      loadFiles(dir);
    } catch (e) {
      status.className = "upstatus err";
      status.textContent = "Feltöltés sikertelen: " + (e.message || e);
    } finally {
      btn.disabled = false;
    }
  };
}

/* ===================================================================== */
/* WebSocket: élő állapot + flash haladás                                */
/* ===================================================================== */

const PHASE_HU = {
  idle: "tétlen", connect: "csatlakozás", erase: "törlés",
  program: "programozás", verify: "ellenőrzés", done: "kész", failed: "hiba",
};

function setConn(on) {
  const c = $("#conn");
  c.dataset.state = on ? "on" : "off";
  $("#connText").textContent = on ? "WS csatlakozva" : "WS leválasztva";
}

function showProg() { $("#progCard").hidden = false; }

function handleState(d) {
  $("#stTarget").textContent  = d.target_present ? "igen" : "nem";
  $("#stDev").textContent     = (d.dev_id != null) ? ("0x" + Number(d.dev_id).toString(16)) : "—";
  $("#stName").textContent    = d.target_name || "—";
  $("#stSerial").textContent  = d.serial_link ? "él" : "nincs";
  $("#stUptime").textContent  = fmtUptime(d.uptime_s);
  $("#stValues").textContent  = Array.isArray(d.values) ? ("[" + d.values.join(", ") + "]") : "—";
  $("#stRaw").textContent     = JSON.stringify(d, null, 2);
  $("#stateAge").textContent  = "frissítve: " + new Date().toLocaleTimeString("hu-HU");
}

function handleProg(d) {
  showProg();
  const phase = d.phase_name || "—";
  const pct = Math.max(0, Math.min(100, Number(d.percent) || 0));
  const badge = $("#progPhase");
  badge.textContent = PHASE_HU[phase] || phase;
  badge.dataset.phase = phase;
  $("#progTarget").textContent = d.target_name ? ("Cél: " + d.target_name) : "";
  $("#progMsg").textContent = d.message || "";
  $("#progFill").style.width = pct + "%";
  $("#progPct").textContent = pct + "%";

  if (phase === "done") { toast("Flash kész.", "ok"); loadFiles("fw"); }
  else if (phase === "failed") { toast("Flash hiba: " + (d.message || ""), "err"); }
}

let ws = null;
let reconnectTimer = null;

function connectWS() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const t = getToken();
  const url = proto + "//" + location.host + "/ws" +
              (t ? "?token=" + encodeURIComponent(t) : "");
  try {
    ws = new WebSocket(url);
  } catch (e) {
    scheduleReconnect();
    return;
  }

  ws.onopen = () => setConn(true);
  ws.onclose = () => { setConn(false); scheduleReconnect(); };
  ws.onerror = () => { setConn(false); };
  ws.onmessage = (ev) => {
    let d;
    try { d = JSON.parse(ev.data); } catch (e) { return; }
    if (d.type === "state") handleState(d);
    else if (d.type === "prog") handleProg(d);
  };
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connectWS, 2000);   // automatikus újracsatlakozás
}

/* ===================================================================== */
/* Indítás                                                              */
/* ===================================================================== */

function setupToken() {
  const input = $("#authToken");
  const save = $("#authSave");
  const clear = $("#authClear");
  if (!input || !save) return;

  input.value = getToken();   // meglévő token betöltése a mezőbe

  save.onclick = () => {
    setToken(input.value.trim());
    toast(input.value.trim() ? "Token mentve" : "Token törölve", "ok");
    // WS újracsatlakozás az új tokennel + listák frissítése
    if (ws) { try { ws.close(); } catch (e) {} }
    connectWS();
    loadFiles("fw");
    loadFiles("cfg");
  };

  if (clear) {
    clear.onclick = () => {
      input.value = "";
      setToken("");
      toast("Token törölve", "ok");
      if (ws) { try { ws.close(); } catch (e) {} }
      connectWS();
      loadFiles("fw");
      loadFiles("cfg");
    };
  }
}

function init() {
  setupToken();
  setupUploader("fw", "fwFile", "fwUpload", "fwUpStatus");
  setupUploader("cfg", "cfgFile", "cfgUpload", "cfgUpStatus");

  document.querySelectorAll("[data-refresh]").forEach((b) => {
    b.onclick = () => loadFiles(b.dataset.refresh);
  });

  loadFiles("fw");
  loadFiles("cfg");
  connectWS();
}

document.addEventListener("DOMContentLoaded", init);
