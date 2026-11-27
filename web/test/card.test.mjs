// Offline tests for the card builder: node --test web/test/
// They build small fake releases in memory, so they need no network.

import { test } from "node:test";
import assert from "node:assert/strict";
import { deflateRawSync } from "node:zlib";
import { readFileSync } from "node:fs";
import { readZip, writeZip, crc32, sha256Hex } from "../public/zip.js";
import { buildCard, allowedAssets, assetId } from "../public/card.js";
import { onRequestGet } from "../functions/api/asset/[id].js";
import { liveManifest, enagaFromBody, clearCache } from "../lib/releases.js";

// No test reaches the network: GitHub's API answers from `api` here, or fails.
let api = null;
const realFetch = globalThis.fetch;
function withFetch(fn) {
  return async () => {
    clearCache();
    try { await fn(); } finally { globalThis.fetch = realFetch; api = null; clearCache(); }
  };
}
function mockGitHub(downloads = () => new Response("not mocked", { status: 599 })) {
  const asked = [];
  globalThis.fetch = async (url) => {
    asked.push(url);
    if (url.startsWith("https://api.github.com/")) {
      const body = api?.[url.replace("https://api.github.com", "")];
      return body === undefined ? new Response("{}", { status: 403 }) : new Response(JSON.stringify(body));
    }
    return downloads(url);
  };
  return asked;
}

const enc = new TextEncoder();
const dec = new TextDecoder();
const manifestFile = JSON.parse(readFileSync(new URL("../public/compat.json", import.meta.url)));

// A zip with deflated entries, the way GitHub's release zips are made.
function deflatedZip(files) {
  const parts = [];
  const central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name);
    const comp = deflateRawSync(f.data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(8, 8);
    lh.writeUInt32LE(crc32(f.data), 14); lh.writeUInt32LE(comp.length, 18); lh.writeUInt32LE(f.data.length, 22);
    lh.writeUInt16LE(name.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(8, 10);
    ch.writeUInt32LE(crc32(f.data), 16); ch.writeUInt32LE(comp.length, 20); ch.writeUInt32LE(f.data.length, 24);
    ch.writeUInt16LE(name.length, 28); ch.writeUInt32LE(offset, 42);
    parts.push(lh, name, comp);
    central.push(ch, name);
    offset += 30 + name.length + comp.length;
  }
  const cd = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...parts, cd, end]));
}

async function fixture({ badSums = false } = {}) {
  const app = new Uint8Array([...enc.encode("ENGA"), 1, 0, 0, 0, ...Array.from({ length: 5000 }, (_, i) => i & 7)]);
  const bin = new Uint8Array(Array.from({ length: 3000 }, (_, i) => (i * 13) & 0xff));
  const appSha = badSums ? "0".repeat(64) : await sha256Hex(app);
  const strataZip = deflatedZip([
    { name: "STRATA-v9.0.0/README.txt", data: enc.encode("readme") },
    { name: "STRATA-v9.0.0/SD-CARD/ENAGA/APPS/STRATA/APP.ENA", data: app },
    { name: "STRATA-v9.0.0/LICENSE.txt", data: enc.encode("MIT License (STRATA)") },
    { name: "STRATA-v9.0.0/THIRD_PARTY_NOTICES.md", data: enc.encode("## Enaga\nMIT License (Enaga)") },
    { name: "STRATA-v9.0.0/SHA256SUMS.txt", data: enc.encode(`${appSha}  SD-CARD/ENAGA/APPS/STRATA/APP.ENA\n`) },
  ]);
  const enagaZip = deflatedZip([{ name: "SmplTrek.bin", data: bin }]);
  const enagaLicence = enc.encode("MIT License\n\nCopyright (c) 2026 seawaffle\n");
  const manifest = {
    schema: 1,
    enaga: { "9.1.0": { tag: "v9.1.0", asset: "enaga-v9.1.0-smpltrek.zip", sha256: await sha256Hex(enagaZip), license_sha256: await sha256Hex(enagaLicence) } },
    strata: [{ version: "9.0.0", tag: "v9.0.0", date: "2030-01-01", asset: "STRATA-v9.0.0-smpltrek.zip", sha256: await sha256Hex(strataZip), enaga: "9.1.0" }],
  };
  const blobs = { "strata-9.0.0": strataZip, "enaga-9.1.0": enagaZip, "enaga-9.1.0-license": enagaLicence };
  const fetched = [];
  const fetchAsset = async (id) => { fetched.push(id); return blobs[id]; };
  return { app, bin, enagaLicence, manifest, blobs, fetchAsset, fetched };
}

test("writeZip output reads back, stored, with correct CRCs", async () => {
  const files = [{ name: "a.txt", data: enc.encode("hello") }, { name: "dir/b.bin", data: new Uint8Array([0, 1, 2, 255]) }];
  const entries = readZip(writeZip(files));
  assert.deepEqual(entries.map((e) => e.name), ["a.txt", "dir/b.bin"]);
  assert.equal(dec.decode(await entries[0].read()), "hello");
  assert.deepEqual([...(await entries[1].read())], [0, 1, 2, 255]);
});

test("writeZip is deterministic", () => {
  const files = [{ name: "x", data: enc.encode("same") }];
  assert.deepEqual(writeZip(files), writeZip(files));
});

test("readZip inflates deflated entries and rejects a corrupted one", async () => {
  const data = enc.encode("abc".repeat(1000));
  const zip = deflatedZip([{ name: "f", data }]);
  assert.equal(dec.decode(await readZip(zip)[0].read()), "abc".repeat(1000));
  const bad = zip.slice();
  const view = new DataView(bad.buffer);
  const central = view.getUint32(bad.length - 22 + 16, true);
  view.setUint32(central + 16, view.getUint32(central + 16, true) ^ 1, true); // the CRC read() checks
  await assert.rejects(readZip(bad)[0].read(), /CRC/);
});

test("a full card has firmware, app, readme and licences at the card root", async () => {
  const fx = await fixture();
  const card = await buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: true, fetchAsset: fx.fetchAsset });
  assert.equal(card.name, "STRATA-v9.0.0-enaga-v9.1.0-sdcard.zip");
  const entries = readZip(card.zip);
  assert.deepEqual(entries.map((e) => e.name),
    ["SmplTrek.bin", "ENAGA/APPS/STRATA/APP.ENA", "STRATA-README.txt", "ENAGA-LICENSE.txt", "STRATA-LICENSES.txt"]);
  assert.deepEqual(await entries[0].read(), fx.bin);
  assert.deepEqual(await entries[1].read(), fx.app);
  const readme = dec.decode(await entries[2].read());
  assert.match(readme, /SYSTEM UPDATE/);
  assert.match(readme, /Enaga 9\.1\.0/);
  assert.deepEqual(await entries[3].read(), fx.enagaLicence);
  const lic = dec.decode(await entries[4].read());
  assert.match(lic, /MIT License \(STRATA\)/);
  assert.match(lic, /MIT License \(Enaga\)/);
  assert.deepEqual(fx.fetched, ["strata-9.0.0", "enaga-9.1.0-license", "enaga-9.1.0"]);
});

test("an app-only card has no firmware and does not download Enaga", async () => {
  const fx = await fixture();
  const card = await buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: false, fetchAsset: fx.fetchAsset });
  const names = readZip(card.zip).map((e) => e.name);
  assert.ok(!names.includes("SmplTrek.bin"));
  assert.ok(names.includes("ENAGA-LICENSE.txt"), "APP.ENA carries Enaga code too, so the licence ships either way");
  assert.equal(card.name, "STRATA-v9.0.0-app-only-sdcard.zip");
  assert.deepEqual(fx.fetched, ["strata-9.0.0", "enaga-9.1.0-license"]);
  const readme = dec.decode(await readZip(card.zip).find((e) => e.name === "STRATA-README.txt").read());
  assert.doesNotMatch(readme, /This installs Enaga/);
  assert.match(readme, /must already run Enaga 9\.1\.0/);
});

test("an Enaga licence that does not match compat.json is refused", async () => {
  const fx = await fixture();
  fx.manifest.enaga["9.1.0"].license_sha256 = "0".repeat(64);
  await assert.rejects(
    buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: false, fetchAsset: fx.fetchAsset }),
    /LICENSE: the download does not match/);
});

test("a download that does not match compat.json's SHA-256 is refused", async () => {
  const fx = await fixture();
  fx.manifest.enaga["9.1.0"].sha256 = "f".repeat(64);
  await assert.rejects(
    buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: true, fetchAsset: fx.fetchAsset }),
    /Enaga 9\.1\.0: the download does not match its published SHA-256/);
});

test("APP.ENA must match the release's SHA256SUMS.txt", async () => {
  const fx = await fixture({ badSums: true });
  await assert.rejects(
    buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: false, fetchAsset: fx.fetchAsset }),
    /SHA256SUMS/);
});

test("compat.json: every STRATA release names a listed Enaga, newest first, no duplicates", () => {
  const m = manifestFile;
  assert.equal(m.schema, 1);
  const seen = new Set();
  const key = (v) => v.split(".").map((n) => n.padStart(4, "0")).join(".");
  for (let i = 0; i < m.strata.length; i++) {
    const s = m.strata[i];
    assert.ok(!seen.has(s.version), `duplicate ${s.version}`);
    seen.add(s.version);
    assert.equal(s.tag, `v${s.version}`);
    assert.equal(s.asset, `STRATA-v${s.version}-smpltrek.zip`);
    assert.match(s.sha256, /^[0-9a-f]{64}$/);
    assert.match(s.date, /^\d{4}-\d{2}-\d{2}$/);
    assert.ok(m.enaga[s.enaga], `STRATA ${s.version} needs Enaga ${s.enaga}, which is not listed`);
    if (i > 0) assert.ok(key(m.strata[i - 1].version) > key(s.version), "list newest first");
  }
  for (const [v, e] of Object.entries(m.enaga)) {
    assert.equal(e.tag, `v${v}`);
    assert.equal(e.asset, `enaga-v${v}-smpltrek.zip`);
    assert.match(e.sha256, /^[0-9a-f]{64}$/);
    assert.match(e.license_sha256, /^[0-9a-f]{64}$/);
  }
});

test("compat.json: the newest STRATA release needs the Enaga that STRATA pins, or an older one", () => {
  // The submodule pin (third_party/enaga) is the Enaga a build from main runs on; INSTALL.md's
  // table says the same. Read the version from Enaga's firmware/build.py, as package_release.py does.
  // Between a pin move and the next release, main is ahead of the newest release (2026-11-27: main
  // on Enaga 0.4.0, release 0.4.0 on Enaga 0.3.0); a release needing a NEWER Enaga than the pin is
  // the mistake this catches.
  let build;
  try {
    build = readFileSync(new URL("../../third_party/enaga/firmware/build.py", import.meta.url), "utf8");
  } catch {
    return; // submodule not checked out
  }
  const m = build.match(/VERSION\s*=\s*\((\d+),\s*(\d+),\s*(\d+)\)/) ?? build.match(/VERSION\s*=\s*["'](\d+)\.(\d+)\.(\d+)["']/);
  assert.ok(m, "cannot read VERSION from Enaga's firmware/build.py");
  const key = (v) => v.split(".").map(Number).reduce((a, n) => a * 1000 + n, 0);
  const pinned = `${m[1]}.${m[2]}.${m[3]}`;
  const released = manifestFile.strata[0].enaga;
  assert.ok(key(released) <= key(pinned), `the newest release needs Enaga ${released}, newer than the pin ${pinned}`);
});

test("the proxy serves only listed files", withFetch(async () => {
  mockGitHub();
  const r = await onRequestGet({ params: { id: "evil" } });
  assert.equal(r.status, 404);
  const proto = await onRequestGet({ params: { id: "__proto__" } });
  assert.equal(proto.status, 404);
  const ids = Object.keys(allowedAssets(manifestFile));
  assert.ok(ids.includes(assetId("strata", manifestFile.strata[0].version)));
  assert.ok(ids.includes(assetId("enaga", manifestFile.strata[0].enaga)));
  assert.ok(ids.includes(assetId("enaga", manifestFile.strata[0].enaga) + "-license"));
}));

test("the proxy fetches the official GitHub download URL and streams it", withFetch(async () => {
  let asked = null;
  const respond = (r) => mockGitHub((url) => { asked = url; return r(); });
  respond(() => new Response(new Uint8Array([1, 2, 3]), { headers: { "content-length": "3" } }));
  {
    const r = await onRequestGet({ params: { id: "enaga-0.3.0" } });
    assert.equal(r.status, 200);
    assert.equal(asked, "https://github.com/seawaffle/enaga/releases/download/v0.3.0/enaga-v0.3.0-smpltrek.zip");
    assert.deepEqual([...new Uint8Array(await r.arrayBuffer())], [1, 2, 3]);
    await onRequestGet({ params: { id: "enaga-0.3.0-license" } });
    assert.equal(asked, "https://raw.githubusercontent.com/seawaffle/enaga/v0.3.0/LICENSE");
    respond(() => new Response("MIT", { headers: { "content-length": "99", "content-encoding": "gzip" } }));
    const gz = await onRequestGet({ params: { id: "enaga-0.3.0-license" } });
    assert.equal(gz.headers.get("content-length"), null, "an encoded length must not reach the browser");
    respond(() => new Response("nope", { status: 404 }));
    assert.equal((await onRequestGet({ params: { id: "strata-0.4.0" } })).status, 502);
  }
}));

const sha = (c) => c.repeat(64);
function release(version, { body = "", digest = sha("a"), draft = false, asset = `STRATA-v${version}-smpltrek.zip` } = {}) {
  return { tag_name: `v${version}`, draft, body, published_at: "2027-01-02T03:04:05Z",
    assets: [{ name: asset, digest: digest && `sha256:${digest}` }, { name: `${asset}.sha256`, digest: `sha256:${sha("f")}` }] };
}

test("enagaFromBody reads the release body's marker, and only a version", () => {
  assert.equal(enagaFromBody('notes\n<!-- strata-card: {"enaga":"0.4.1"} -->\n'), "0.4.1");
  assert.equal(enagaFromBody("no marker"), null);
  assert.equal(enagaFromBody('<!-- strata-card: {"enaga":"../evil"} -->'), null);
  assert.equal(enagaFromBody("<!-- strata-card: {broken} -->"), null);
});

test("a new release appears from GitHub with its digest and its marked Enaga", withFetch(async () => {
  api = {
    "/repos/Devko/STRATA/releases?per_page=100": [
      release("0.5.0", { body: '<!-- strata-card: {"enaga":"0.4.0"} -->', digest: sha("b") }),
      release("0.4.0", { digest: manifestFile.strata[0].sha256 }),
      release("0.3.9", { body: "no marker, not pinned" }),
      release("0.3.8", { body: '<!-- strata-card: {"enaga":"0.3.0"} -->', draft: true }),
      release("0.3.7", { body: '<!-- strata-card: {"enaga":"0.3.0"} -->', digest: null }),
    ],
    "/repos/seawaffle/enaga/releases/tags/v0.4.0": { draft: false, assets: [{ name: "enaga-v0.4.0-smpltrek.zip", digest: `sha256:${sha("c")}` }] },
  };
  mockGitHub();
  const m = await liveManifest();
  assert.equal(m.source, "github");
  assert.deepEqual(m.strata.map((s) => s.version), ["0.5.0", "0.4.0"]);
  assert.deepEqual(m.strata[0], { version: "0.5.0", tag: "v0.5.0", date: "2027-01-02", asset: "STRATA-v0.5.0-smpltrek.zip", sha256: sha("b"), enaga: "0.4.0" });
  assert.equal(m.strata[1].enaga, "0.3.0", "pinned releases keep their pinned Enaga");
  assert.deepEqual(m.enaga["0.4.0"], { tag: "v0.4.0", asset: "enaga-v0.4.0-smpltrek.zip", sha256: sha("c") });
  assert.deepEqual(m.enaga["0.3.0"], manifestFile.enaga["0.3.0"]);
  const ids = Object.keys(allowedAssets(m));
  assert.ok(ids.includes("strata-0.5.0") && ids.includes("enaga-0.4.0") && ids.includes("enaga-0.4.0-license"));
}));

test("a pinned release whose GitHub digest changed is not offered", withFetch(async () => {
  api = { "/repos/Devko/STRATA/releases?per_page=100": [release("0.4.0", { digest: sha("d") }), release("0.3.1", { digest: manifestFile.strata[1].sha256 })] };
  mockGitHub();
  assert.deepEqual((await liveManifest()).strata.map((s) => s.version), ["0.3.1"]);
}));

test("a release whose Enaga cannot be found is not offered", withFetch(async () => {
  api = { "/repos/Devko/STRATA/releases?per_page=100": [release("0.5.0", { body: '<!-- strata-card: {"enaga":"9.9.9"} -->' }), release("0.4.0", { digest: manifestFile.strata[0].sha256 })] };
  mockGitHub();
  assert.deepEqual((await liveManifest()).strata.map((s) => s.version), ["0.4.0"]);
}));

test("without GitHub's API the pinned list is served", withFetch(async () => {
  mockGitHub();
  const m = await liveManifest();
  assert.equal(m.source, "pinned");
  assert.deepEqual(m.strata, manifestFile.strata);
}));

test("GitHub's answer is cached, so the API is asked once", withFetch(async () => {
  api = { "/repos/Devko/STRATA/releases?per_page=100": [release("0.4.0", { digest: manifestFile.strata[0].sha256 })] };
  const asked = mockGitHub();
  await liveManifest();
  await liveManifest();
  assert.equal(asked.filter((u) => u.includes("/repos/Devko/STRATA/releases")).length, 1);
}));

test("a GITHUB_TOKEN, if the site has one, goes to GitHub's API only", withFetch(async () => {
  const seen = [];
  globalThis.fetch = async (url, init) => { seen.push([url, init?.headers?.authorization]); return new Response("[]"); };
  await liveManifest({ GITHUB_TOKEN: "t0ken" });
  assert.deepEqual(seen, [["https://api.github.com/repos/Devko/STRATA/releases?per_page=100", "Bearer t0ken"]]);
}));
