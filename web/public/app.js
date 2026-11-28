import { buildCard, releaseUrl, REPOS, starterFits } from "./card.js";

const $ = (id) => document.getElementById(id);
const select = $("version");
const go = $("go");
const log = $("log");
let manifest = null;
let starter = null;   // public/starter/starter.json; null if the site has none
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
  showStarter();
}

// The starter section for the picked version: projects and the quick start need
// starter.min_strata, the sample pack fits any. One box takes all of it or none.
function showStarter() {
  if (!starter) return;
  const fits = starterFits(starter, select.value);
  $("starter-projects-row").hidden = !fits.projects;
  $("starter-qs-row").hidden = !fits.quickstart;
  const note = $("starter-note");
  note.hidden = fits.projects;
  note.textContent = fits.projects ? "" : `The starter projects and the quick start need STRATA ${starter.min_strata} or newer, so this version gets the sample pack only.`;
  updateStarterSteps();
}

function picked() {
  if (!starter) return null;
  const on = $("starter-on").checked;
  return { projects: on ? starter.projects.map((p) => p.name) : [], samples: on, quickstart: on };
}

function updateStarterSteps() {
  const fits = starterFits(starter, select.value);
  const on = $("starter-on").checked;
  $("step-starter").hidden = !(fits.projects && on);
  $("starter-list").classList.toggle("off", !on);
  $("result").hidden = true;
}

function setupStarter() {
  $("starter-projects").textContent = starter.projects.map((p) => p.name).join(", ");
  $("starter-on").addEventListener("change", updateStarterSteps);
  const mb = (starter.samples.unpacked / 1e6).toFixed(1);
  $("starter-samples-info").textContent = `${starter.samples.count} one-shots and chords, ${mb} MB`;
  const zipped = starter.samples.bytes + starter.projects.reduce((n, p) => n + p.bytes, 0);
  $("starter-size").textContent = `(about ${Math.round(zipped / 1e6)} MB more to download)`;
  // The build and install steps move down one.
  for (const n of document.querySelectorAll("h2 .num")) n.textContent = String(Number(n.textContent) + 1);
  $("starter-section").hidden = false;
}

async function fetchStarter(file) {
  const r = await fetch(`starter/${encodeURIComponent(file)}`);
  if (!r.ok) throw new Error(`starter content download failed (${r.status})`);
  return new Uint8Array(await r.arrayBuffer());
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
    const card = await buildCard({
      manifest, strataVersion: select.value, includeFirmware, fetchAsset, onStep: (t) => say(t),
      starter, pick: picked(), fetchStarter,
    });
    say("Every download matches its published SHA-256. Ready.", "ok");
    lastUrl = URL.createObjectURL(new Blob([card.zip], { type: "application/zip" }));
    const save = $("save");
    save.href = lastUrl;
    save.download = card.name;
    save.textContent = `Save ${card.name}`;
    // A sample pack is a hundred lines; one line says it.
    const shown = card.files.filter((f) => !f.name.startsWith("SAMPLES/") && !/^ENAGA\/DATA\//.test(f.name));
    const summary = [];
    if (card.starter.projects.length) summary.push({ name: `ENAGA/DATA/STRATA/PROJECTS/: ${card.starter.projects.join(", ")}`, size: sizeOf(card.files, "ENAGA/DATA/") });
    if (card.starter.samples) summary.push({ name: `SAMPLES/STRATA/: ${card.starter.samples} WAV files`, size: sizeOf(card.files, "SAMPLES/") });
    $("files").replaceChildren(...[...shown, ...summary].map((f) => {
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

function sizeOf(files, prefix) {
  return files.filter((f) => f.name.startsWith(prefix)).reduce((n, f) => n + f.size, 0);
}

async function init() {
  try {
    // The live list from GitHub; the pinned list in compat.json if the site's API is not there.
    let r = await fetch("api/releases").catch(() => null);
    if (!r || !r.ok) r = await fetch("compat.json", { cache: "no-cache" });
    manifest = await r.json();
    // Starter content is optional: a site without it builds plain cards.
    const st = await fetch("starter/starter.json", { cache: "no-cache" }).catch(() => null);
    starter = st && st.ok ? await st.json().catch(() => null) : null;
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
  if (starter) setupStarter();
  showPair();
}

init();
