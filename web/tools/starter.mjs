// Packs the starter content for the card builder into public/starter/ (web/README.md,
// "Starter content"):
//
//   public/starter/<NAME>.zip      one project, ENAGA/DATA/STRATA/PROJECTS/<NAME>/*.BIN, deflated
//   public/starter/samples.zip     the sample pack, SAMPLES/STRATA/<FOLDER>/*.WAV, deflated
//   public/starter/QUICKSTART.txt  the one-page quick start
//   public/starter/starter.json    what the page offers, with every file's SHA-256 and size
//
//   node tools/starter.mjs                    projects and quick start from ../content/starter;
//                                             the sample pack as it is in public/starter
//   node tools/starter.mjs --samples DIR      and the sample pack repacked from DIR, the
//                                             output of `build/host/strata-starter samples DIR`
//
// The projects come from ../content/starter/projects/, written by `strata-starter resave` (or by
// tools/showcases/build.sh --starter), and tests/test_starter.cpp opens each of them through the
// app's real open path. The output is byte-identical for the same input, so a rerun with nothing
// changed changes no file, and `npm test` checks that starter.json matches the files.

import { readFileSync, writeFileSync, readdirSync, statSync, mkdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import { deflateRawSync } from "node:zlib";
import { writeZip, crc32, readZip } from "../public/zip.js";

const WEB = fileURLToPath(new URL("..", import.meta.url));
const CONTENT = join(WEB, "..", "content", "starter");
const OUT = join(WEB, "public", "starter");

// The oldest STRATA the projects and the quick start are for: file format C (the LFO layout)
// came with 0.5.0, and a 0.4.0 reads these projects' LFOs wrong (CHANGELOG, 0.5.0).
export const MIN_STRATA = "0.5.0";

const sha256 = (b) => createHash("sha256").update(b).digest("hex");

function deflatedEntry(name, data) {
  return { name, size: data.length, crc: crc32(data), method: 8, compressed: new Uint8Array(deflateRawSync(data, { level: 9 })) };
}

function filesUnder(dir, prefix = "") {
  const out = [];
  for (const n of readdirSync(dir).sort()) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) out.push(...filesUnder(p, `${prefix}${n}/`));
    else out.push({ rel: `${prefix}${n}`, path: p });
  }
  return out;
}

function write(name, bytes) {
  writeFileSync(join(OUT, name), bytes);
  return { file: name, bytes: bytes.length, sha256: sha256(bytes) };
}

export function main(argv) {
  const i = argv.indexOf("--samples");
  const samplesDir = i >= 0 ? argv[i + 1] : null;
  mkdirSync(OUT, { recursive: true });
  const meta = JSON.parse(readFileSync(join(CONTENT, "projects.json"), "utf8"));

  const projects = meta.projects.map((p) => {
    const dir = join(CONTENT, "projects", p.name);
    const files = filesUnder(dir);
    if (!files.some((f) => f.rel === "PROJECT.BIN")) throw new Error(`${p.name}: no PROJECT.BIN in ${dir}`);
    const zip = writeZip(files.map((f) => deflatedEntry(`ENAGA/DATA/STRATA/PROJECTS/${p.name}/${f.rel}`, readFileSync(f.path))));
    return { ...p, ...write(`${p.name}.zip`, zip) };
  });
  const listed = new Set(meta.projects.map((p) => p.name));
  for (const n of readdirSync(join(CONTENT, "projects"))) {
    if (!listed.has(n)) throw new Error(`content/starter/projects/${n} is not in projects.json`);
  }

  let samples;
  if (samplesDir) {
    const files = filesUnder(samplesDir).filter((f) => /\.WAV$/i.test(f.rel));
    if (files.length === 0) throw new Error(`no WAV files under ${samplesDir}`);
    const zip = writeZip(files.map((f) => deflatedEntry(`SAMPLES/STRATA/${f.rel}`, readFileSync(f.path))));
    samples = { ...write("samples.zip", zip), count: files.length };
  } else {
    const p = join(OUT, "samples.zip");
    if (!existsSync(p)) throw new Error("public/starter/samples.zip is missing: run with --samples DIR");
    const zip = readFileSync(p);
    samples = { file: "samples.zip", bytes: zip.length, sha256: sha256(zip), count: readZip(new Uint8Array(zip)).length };
  }
  samples.unpacked = readZip(new Uint8Array(readFileSync(join(OUT, "samples.zip")))).reduce((n, e) => n + e.size, 0);

  const quickstart = write("QUICKSTART.txt", readFileSync(join(CONTENT, "QUICKSTART.txt")));

  const manifest = {
    _comment: "Written by web/tools/starter.mjs from content/starter; do not edit by hand.",
    min_strata: MIN_STRATA,
    projects,
    samples,
    quickstart,
  };
  writeFileSync(join(OUT, "starter.json"), JSON.stringify(manifest, null, 2) + "\n");
  console.log(`${projects.length} projects, ${samples.count} samples (${(samples.bytes / 1e6).toFixed(1)} MB zipped), quick start -> ${OUT}`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main(process.argv.slice(2));
