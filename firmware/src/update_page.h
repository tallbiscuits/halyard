// SPDX-License-Identifier: GPL-3.0-or-later
// GET /halyard/: updates from a browser, on any OS (no bash, curl or Docker). Built into the
// firmware rather than kept on the card, so it works with no Mainsail, or a broken one.
//   - Firmware: a firmware.bin, checked here (an ESP32-S3 app image) and again by the
//     firmware, then POSTed to /halyard/update. The page then waits for the new version,
//     and its requests confirm the trial image.
//   - Mainsail: Mainsail's release mainsail.zip, or a Halyard card zip. Unzipped and gzipped
//     in the browser (DecompressionStream / CompressionStream) into the ustar tar that
//     POST /halyard/webui takes, the same bytes ./run.sh webui sends.
// test/js/test_update_page.mjs runs the <script> below in Node against a real mainsail.zip.
#pragma once

static const char kUpdatePage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Halyard updates</title>
<style>
:root { --bg:#f6f7f9; --card:#fff; --fg:#1d2330; --muted:#5d6675; --line:#dde1e7;
        --accent:#2f6fde; --ok:#1f8a4c; --bad:#c23b31; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#15181d; --card:#1e2229; --fg:#e6e9ee; --muted:#9aa3b1; --line:#323843;
          --accent:#6b9cf0; --ok:#4cc27f; --bad:#f07267; } }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:16px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main { max-width:42rem; margin:0 auto; padding:24px 16px 48px; }
header { display:flex; align-items:baseline; justify-content:space-between; gap:12px; flex-wrap:wrap; }
h1 { font-size:1.4rem; margin:0; }
h2 { font-size:1.1rem; margin:0 0 4px; }
a { color:var(--accent); }
.muted { color:var(--muted); font-size:.92rem; }
section { background:var(--card); border:1px solid var(--line); border-radius:10px;
          padding:16px; margin-top:16px; }
.row { display:flex; gap:10px; flex-wrap:wrap; align-items:center; margin-top:12px; }
input[type=file] { flex:1 1 16rem; min-width:0; color:var(--fg); }
button { font:inherit; padding:7px 16px; border-radius:7px; border:0; background:var(--accent);
         color:#fff; cursor:pointer; }
button:disabled { opacity:.45; cursor:default; }
progress { width:100%; height:8px; margin-top:12px; accent-color:var(--accent); }
.status { margin-top:8px; min-height:1.5em; white-space:pre-wrap; }
.ok { color:var(--ok); } .bad { color:var(--bad); }
code { font-size:.9em; }
</style></head>
<body><main>
<header><h1>Halyard updates</h1><a href="/">Open Mainsail</a></header>
<p class="muted" id="running">Checking the running version…</p>

<section>
  <h2>Firmware</h2>
  <div class="muted">A <code>firmware.bin</code> from a Halyard release. The new firmware
  starts on trial: if it fails to come back, Halyard returns to this one. A print carries on
  during an update, but one is refused while a file is being copied to the printer.</div>
  <div class="row"><input type="file" id="fw-file" accept=".bin">
  <button id="fw-go" disabled>Update firmware</button></div>
  <progress id="fw-bar" value="0" max="1" hidden></progress>
  <div class="status" id="fw-status"></div>
</section>

<section>
  <h2>Mainsail</h2>
  <p class="muted" id="ui-now"></p>
  <div class="muted">With internet, Mainsail updates itself: Machine, then Update Manager.
  Here, install one from a file instead: Mainsail's <code>mainsail.zip</code> from
  <a href="https://github.com/mainsail-crew/mainsail/releases" target="_blank" rel="noopener">its
  releases</a>, or a Halyard card zip. The old one stays until the new one is complete.</div>
  <div class="row"><input type="file" id="ui-file" accept=".zip">
  <button id="ui-go" disabled>Install Mainsail</button></div>
  <progress id="ui-bar" value="0" max="1" hidden></progress>
  <div class="status" id="ui-status"></div>
  <div class="row" id="prev-row" hidden><button id="prev-go"></button>
  <span class="muted">if a new Mainsail does not work with this Halyard.</span></div>
</section>
</main>
<script>
"use strict";
// ---- pure helpers (also run by test/js/test_update_page.mjs) ------------------------------
const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

// An ESP32-S3 app image: the image magic, the chip, and the app description after the first
// segment header. A merged image for USB flashing starts with the bootloader and fails here.
function checkFirmware(b) {
  if (b.length < 256 || b[0] !== 0xE9) return { error: "This is not an ESP32 firmware image." };
  if (u16(b, 12) !== 9) return { error: "This image is for another ESP32 chip, not the ESP32-S3." };
  if (u32(b, 32) !== 0xABCD5432)
    return { error: "This is not an app image. For a USB flash image, use the web flasher; here, use firmware.bin." };
  return {};  // (the app description's date is the Arduino core's, not Halyard's build)
}

async function through(bytes, stream) {
  return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(stream)).arrayBuffer());
}

// Every file in a zip, as [name, bytes]. Stored and deflated entries (all that zip tools write).
async function unzip(b) {
  let e = b.length - 22;
  while (e >= 0 && u32(b, e) !== 0x06054b50) e--;
  if (e < 0) throw new Error("This is not a zip file.");
  const count = u16(b, e + 10);
  let p = u32(b, e + 16);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (u32(b, p) !== 0x02014b50) throw new Error("The zip file is damaged.");
    const method = u16(b, p + 10), size = u32(b, p + 20), nlen = u16(b, p + 28);
    const skip = nlen + u16(b, p + 30) + u16(b, p + 32), local = u32(b, p + 42);
    const name = new TextDecoder().decode(b.subarray(p + 46, p + 46 + nlen));
    p += 46 + skip;
    if (name.endsWith("/")) continue;
    const start = local + 30 + u16(b, local + 26) + u16(b, local + 28);
    const raw = b.subarray(start, start + size);
    if (method === 0) out.push([name, raw]);
    else if (method === 8) out.push([name, await through(raw, new DecompressionStream("deflate-raw"))]);
    else throw new Error(name + ": a zip compression this page cannot read.");
  }
  return out;
}

// Mainsail's files as Halyard stores them: each gzipped where that saves at least a tenth
// (as ./run.sh webui). A Halyard card zip has them ready under .halyard/www/.
async function mainsailFiles(entries) {
  const card = ".halyard/www/";
  const fromCard = entries.filter(([n]) => n.startsWith(card));
  if (fromCard.length) return fromCard.map(([n, d]) => [n.slice(card.length), d]);
  if (!entries.some(([n]) => n === "index.html"))
    throw new Error("No index.html in this zip: is it Mainsail's mainsail.zip?");
  const out = [];
  for (const [n, d] of entries) {
    const z = await through(d, new CompressionStream("gzip"));
    out.push(z.length * 10 < d.length * 9 ? [n + ".gz", z] : [n, d]);
  }
  return out;
}

// A ustar archive (what WebUi::Install reads).
function tar(files) {
  const enc = new TextEncoder();
  const parts = [];
  let total = 1024;
  for (const [, d] of files) total += 512 + Math.ceil(d.length / 512) * 512;
  const t = new Uint8Array(total);
  let p = 0;
  const put = (o, s) => t.set(enc.encode(s), p + o);
  const oct = (v, w) => v.toString(8).padStart(w - 1, "0") + "\0";
  for (const [name, d] of files) {
    let n = name, prefix = "";
    if (enc.encode(n).length > 100) {
      const cut = n.lastIndexOf("/", 155);
      if (cut < 0 || enc.encode(n.slice(cut + 1)).length > 100) throw new Error(name + ": name too long");
      prefix = n.slice(0, cut); n = n.slice(cut + 1);
    }
    put(0, n); put(100, "0000644\0"); put(108, "0000000\0"); put(116, "0000000\0");
    put(124, oct(d.length, 12)); put(136, oct(0, 12)); put(148, "        ");
    put(156, "0"); put(257, "ustar\0"); put(263, "00"); put(345, prefix);
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += t[p + i];
    put(148, sum.toString(8).padStart(6, "0") + "\0 ");
    t.set(d, p + 512);
    p += 512 + Math.ceil(d.length / 512) * 512;
  }
  return t;  // ends in two zero blocks
}

if (typeof document !== "undefined") {
// ---- the page ------------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);
function say(which, msg, cls) { const s = $(which + "-status"); s.textContent = msg; s.className = "status " + (cls || ""); }
function bar(which, v) { const b = $(which + "-bar"); b.hidden = v === null; if (v !== null) b.value = v; }

async function version() {
  try {
    const r = await fetch("/api/version", { cache: "no-store" });
    const m = /\(Halyard ([^)]*)\)/.exec((await r.json()).text || "");
    return m ? m[1] : null;
  } catch (e) { return null; }
}
async function uptime() {
  try {
    const r = await fetch("/machine/proc_stats", { cache: "no-store" });
    return (await r.json()).result.system_uptime;
  } catch (e) { return null; }
}
async function showMainsail() {
  try {
    const i = await (await fetch("/halyard/info", { cache: "no-store" })).json();
    $("ui-now").textContent = i.mainsail ? "Installed: Mainsail " + i.mainsail + "." : "Mainsail is not installed.";
    $("prev-row").hidden = !i.previous;
    $("prev-go").textContent = "Go back to Mainsail " + i.previous;
  } catch (e) {}
}
$("prev-go") && ($("prev-go").onclick = async () => {
  $("prev-go").disabled = true;
  const r = await post("/halyard/webui/restore", "", "text/plain", "ui");
  bar("ui", null);
  $("prev-go").disabled = false;
  say("ui", r.ok ? "Done. Reload Mainsail to use it." : "Not restored: " + r.msg, r.ok ? "ok" : "bad");
  showMainsail();
});
async function showRunning() {
  const v = await version();
  $("running").textContent = v ? "Running Halyard " + v + "." : "Halyard is not answering.";
  return v;
}

function post(url, body, type, which) {
  return new Promise((resolve) => {
    const x = new XMLHttpRequest();
    x.open("POST", url);
    x.setRequestHeader("Content-Type", type);
    x.upload.onprogress = (e) => { if (e.lengthComputable) bar(which, e.loaded / e.total); };
    x.upload.onload = () => bar(which, 1);
    x.onload = () => {
      let msg = x.responseText;
      try { const j = JSON.parse(msg); msg = (j.error && (j.error.message || j.error)) || msg; } catch (e) {}
      resolve({ ok: x.status >= 200 && x.status < 300, status: x.status, msg });
    };
    x.onerror = () => resolve({ ok: false, status: 0, msg: "the connection failed" });
    x.send(body);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

for (const which of ["fw", "ui"])
  $(which + "-file").onchange = () => { $(which + "-go").disabled = !$(which + "-file").files.length; say(which, ""); };

$("fw-go").onclick = async () => {
  const f = $("fw-file").files[0];
  const b = new Uint8Array(await f.arrayBuffer());
  const c = checkFirmware(b);
  if (c.error) return say("fw", c.error, "bad");
  $("fw-go").disabled = true;
  const before = await showRunning();
  say("fw", "Sending " + f.name + " (" + (b.length / 1048576).toFixed(2) + " MB)…");
  bar("fw", 0);
  const r = await post("/halyard/update", b, "application/octet-stream", "fw");
  if (!r.ok) { bar("fw", null); $("fw-go").disabled = false; return say("fw", "Refused: " + r.msg, "bad"); }
  say("fw", "Written. Halyard is restarting…");
  // Back means restarted: an uptime shorter than the time since the upload. The old firmware
  // may still answer for a moment before it restarts.
  const sent = Date.now();
  for (let i = 0; i < 90; i++) {
    await sleep(2000);
    const up = await uptime();
    if (up === null || up > (Date.now() - sent) / 1000) continue;
    const now = await version();  // answering confirms the new firmware (it is on trial)
    if (!now) continue;
    bar("fw", null); showRunning(); $("fw-go").disabled = false;
    return now !== before ? say("fw", "Done: now running " + now + ".", "ok")
      : say("fw", "Halyard restarted, still on " + now + ". Either this was the same firmware, or the new one failed and Halyard returned to the old one.", "bad");
  }
  bar("fw", null); $("fw-go").disabled = false;
  say("fw", "Halyard has not come back after 3 minutes. Check that it is powered, and look at the printer's screen for its address.", "bad");
};

$("ui-go").onclick = async () => {
  const f = $("ui-file").files[0];
  $("ui-go").disabled = true;
  try {
    say("ui", "Unpacking " + f.name + "…");
    const files = await mainsailFiles(await unzip(new Uint8Array(await f.arrayBuffer())));
    const t = tar(files);
    say("ui", "Sending " + files.length + " files (" + (t.length / 1048576).toFixed(1) + " MB)…");
    bar("ui", 0);
    const x = post("/halyard/webui", t, "application/x-tar", "ui");
    const wait = setInterval(() => { if ($("ui-bar").value >= 1) say("ui", "Installing on Halyard's card (this takes about half a minute)…"); }, 500);
    const r = await x;
    clearInterval(wait);
    bar("ui", null);
    if (!r.ok) return say("ui", "Not installed: " + r.msg, "bad");
    say("ui", "Installed. Reload Mainsail to use it.", "ok");
    showMainsail();
  } catch (e) {
    bar("ui", null);
    say("ui", e.message, "bad");
  } finally {
    $("ui-go").disabled = false;
  }
};

showRunning();
showMainsail();
}
</script>
</body></html>
)HTML";
