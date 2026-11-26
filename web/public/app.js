import { buildCard, releaseUrl, REPOS } from "./card.js";

const $ = (id) => document.getElementById(id);
const select = $("version");
const go = $("go");
const log = $("log");
let manifest = null;
let lastUrl = null;

function say(text, cls) {
  const li = document.createElement("li");
  li.textContent = text;
  if (cls) li.className = cls;
  log.append(li);
}

function link(href, text) {
  const a = document.createElement("a");
  a.href = href;
  a.textContent = text;
  return a;
}

function showPair() {
  const s = manifest.strata.find((x) => x.version === select.value);
  const e = manifest.enaga[s.enaga];
  const pair = $("pair");
  pair.replaceChildren(
    "Needs ", link(releaseUrl(REPOS.enaga, e.tag), `Enaga ${s.enaga}`),
    " exactly. ", link(releaseUrl(REPOS.strata, s.tag), "Release notes"), ".",
  );
  const appOnly = document.querySelector('input[name="fw"]:checked').value === "app";
  $("step-fw").hidden = appOnly;
  $("result").hidden = true;
}

async function fetchAsset(id) {
  const r = await fetch(`api/asset/${encodeURIComponent(id)}`);
  if (!r.ok) throw new Error(`download failed (${r.status}): ${(await r.text()).trim()}`);
  return new Uint8Array(await r.arrayBuffer());
}

async function build() {
  go.disabled = true;
  log.replaceChildren();
  $("result").hidden = true;
  if (lastUrl) URL.revokeObjectURL(lastUrl);
  try {
    if (typeof DecompressionStream === "undefined") {
      throw new Error("this browser cannot unzip (no DecompressionStream). Use a current Chrome, Edge, Firefox or Safari.");
    }
    const includeFirmware = document.querySelector('input[name="fw"]:checked').value === "full";
    const card = await buildCard({ manifest, strataVersion: select.value, includeFirmware, fetchAsset, onStep: (t) => say(t) });
    say("Every download matches its published SHA-256. Ready.", "ok");
    lastUrl = URL.createObjectURL(new Blob([card.zip], { type: "application/zip" }));
    const save = $("save");
    save.href = lastUrl;
    save.download = card.name;
    save.textContent = `Save ${card.name}`;
    $("files").replaceChildren(...card.files.map((f) => {
      const li = document.createElement("li");
      const code = document.createElement("code");
      code.textContent = f.name;
      li.append(code, ` (${f.size.toLocaleString("en")} bytes)`);
      return li;
    }));
    $("result").hidden = false;
  } catch (err) {
    say(`Stopped: ${err.message}`, "err");
  } finally {
    go.disabled = false;
  }
}

async function init() {
  try {
    const r = await fetch("compat.json", { cache: "no-cache" });
    manifest = await r.json();
  } catch (err) {
    select.replaceChildren(new Option("Could not load the release list"));
    say(`Stopped: ${err.message}`, "err");
    return;
  }
  select.replaceChildren(...manifest.strata.map((s, i) =>
    new Option(`STRATA ${s.version}${i === 0 ? " (latest)" : ""}, ${s.date}, with Enaga ${s.enaga}`, s.version)));
  select.disabled = false;
  go.disabled = false;
  select.addEventListener("change", showPair);
  for (const r of document.querySelectorAll('input[name="fw"]')) r.addEventListener("change", showPair);
  go.addEventListener("click", build);
  showPair();
}

init();
