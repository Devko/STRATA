// Offline tests for the card builder: node --test web/test/
// They build small fake releases in memory, so they need no network.

import { test } from "node:test";
import assert from "node:assert/strict";
import { deflateRawSync } from "node:zlib";
import { readFileSync } from "node:fs";
import { readZip, writeZip, crc32, sha256Hex } from "../public/zip.js";
import { buildCard, allowedAssets, assetId } from "../public/card.js";
import { onRequestGet } from "../functions/api/asset/[id].js";

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
  const manifest = {
    schema: 1,
    enaga: { "9.1.0": { tag: "v9.1.0", asset: "enaga-v9.1.0-smpltrek.zip", sha256: await sha256Hex(enagaZip) } },
    strata: [{ version: "9.0.0", tag: "v9.0.0", date: "2030-01-01", asset: "STRATA-v9.0.0-smpltrek.zip", sha256: await sha256Hex(strataZip), enaga: "9.1.0" }],
  };
  const blobs = { "strata-9.0.0": strataZip, "enaga-9.1.0": enagaZip };
  const fetched = [];
  const fetchAsset = async (id) => { fetched.push(id); return blobs[id]; };
  return { app, bin, manifest, blobs, fetchAsset, fetched };
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
    ["SmplTrek.bin", "ENAGA/APPS/STRATA/APP.ENA", "STRATA-README.txt", "STRATA-LICENSES.txt"]);
  assert.deepEqual(await entries[0].read(), fx.bin);
  assert.deepEqual(await entries[1].read(), fx.app);
  const readme = dec.decode(await entries[2].read());
  assert.match(readme, /SYSTEM UPDATE/);
  assert.match(readme, /Enaga 9\.1\.0/);
  const lic = dec.decode(await entries[3].read());
  assert.match(lic, /MIT License \(STRATA\)/);
  assert.match(lic, /MIT License \(Enaga\)/);
  assert.deepEqual(fx.fetched, ["strata-9.0.0", "enaga-9.1.0"]);
});

test("an app-only card has no firmware and does not download Enaga", async () => {
  const fx = await fixture();
  const card = await buildCard({ manifest: fx.manifest, strataVersion: "9.0.0", includeFirmware: false, fetchAsset: fx.fetchAsset });
  const names = readZip(card.zip).map((e) => e.name);
  assert.ok(!names.includes("SmplTrek.bin"));
  assert.equal(card.name, "STRATA-v9.0.0-app-only-sdcard.zip");
  assert.deepEqual(fx.fetched, ["strata-9.0.0"]);
  const readme = dec.decode(await readZip(card.zip).find((e) => e.name === "STRATA-README.txt").read());
  assert.doesNotMatch(readme, /This installs Enaga/);
  assert.match(readme, /must already run Enaga 9\.1\.0/);
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
  }
});

test("compat.json: the newest STRATA release needs the Enaga that STRATA pins", () => {
  // The submodule pin (third_party/enaga) is the Enaga a build from main runs on; INSTALL.md's
  // table says the same. Read the version from Enaga's firmware/build.py, as package_release.py does.
  let build;
  try {
    build = readFileSync(new URL("../../third_party/enaga/firmware/build.py", import.meta.url), "utf8");
  } catch {
    return; // submodule not checked out
  }
  const m = build.match(/VERSION\s*=\s*\((\d+),\s*(\d+),\s*(\d+)\)/) ?? build.match(/VERSION\s*=\s*["'](\d+)\.(\d+)\.(\d+)["']/);
  assert.ok(m, "cannot read VERSION from Enaga's firmware/build.py");
  assert.equal(manifestFile.strata[0].enaga, `${m[1]}.${m[2]}.${m[3]}`);
});

test("the proxy serves only listed files", async () => {
  const r = await onRequestGet({ params: { id: "evil" } });
  assert.equal(r.status, 404);
  const proto = await onRequestGet({ params: { id: "__proto__" } });
  assert.equal(proto.status, 404);
  const ids = Object.keys(allowedAssets(manifestFile));
  assert.ok(ids.includes(assetId("strata", manifestFile.strata[0].version)));
  assert.ok(ids.includes(assetId("enaga", manifestFile.strata[0].enaga)));
});

test("the proxy fetches the official GitHub download URL and streams it", async () => {
  const real = globalThis.fetch;
  let asked = null;
  globalThis.fetch = async (url) => { asked = url; return new Response(new Uint8Array([1, 2, 3]), { headers: { "content-length": "3" } }); };
  try {
    const r = await onRequestGet({ params: { id: "enaga-0.3.0" } });
    assert.equal(r.status, 200);
    assert.equal(asked, "https://github.com/seawaffle/enaga/releases/download/v0.3.0/enaga-v0.3.0-smpltrek.zip");
    assert.deepEqual([...new Uint8Array(await r.arrayBuffer())], [1, 2, 3]);
    globalThis.fetch = async () => new Response("nope", { status: 404 });
    assert.equal((await onRequestGet({ params: { id: "strata-0.4.0" } })).status, 502);
  } finally {
    globalThis.fetch = real;
  }
});
