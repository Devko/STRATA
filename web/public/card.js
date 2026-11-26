// Builds the SD card zip from the two official releases. No DOM here, so the tests run it in Node.
//
// fetchAsset(id) returns the bytes of one release zip; in the browser it goes through the
// site's /api/asset/<id> proxy, because GitHub's release downloads send no CORS headers.
// Nothing is trusted from the network: each zip must match the SHA-256 in compat.json, and
// APP.ENA must also match the release's own SHA256SUMS.txt and start with Enaga's package magic.

import { readZip, writeZip, sha256Hex } from "./zip.js";

export const REPOS = { strata: "Devko/STRATA", enaga: "seawaffle/enaga" };
const APP_SUFFIX = "SD-CARD/ENAGA/APPS/STRATA/APP.ENA";
const CARD_APP = "ENAGA/APPS/STRATA/APP.ENA";

export function assetId(kind, version) {
  return `${kind}-${version}`;
}

function download(repo, tag, asset) {
  return `https://github.com/${repo}/releases/download/${tag}/${asset}`;
}

// Every file the manifest allows, by id: { url, name, sha256 }. Release zips come from the
// release's downloads, Enaga's licence from the repository at the release's tag.
export function allowedAssets(manifest) {
  const out = {};
  for (const [version, e] of Object.entries(manifest.enaga)) {
    out[assetId("enaga", version)] = { url: download(REPOS.enaga, e.tag, e.asset), name: e.asset, sha256: e.sha256 };
    out[assetId("enaga", version) + "-license"] = {
      url: `https://raw.githubusercontent.com/${REPOS.enaga}/${e.tag}/LICENSE`,
      name: "LICENSE",
      sha256: e.license_sha256,
    };
  }
  for (const s of manifest.strata) {
    out[assetId("strata", s.version)] = { url: download(REPOS.strata, s.tag, s.asset), name: s.asset, sha256: s.sha256 };
  }
  return out;
}

export function releaseUrl(repo, tag) {
  return `https://github.com/${repo}/releases/tag/${tag}`;
}

async function verified(fetchAsset, id, expected, label) {
  const bytes = await fetchAsset(id);
  if (expected === undefined) return bytes; // only Enaga's LICENSE of an unpinned release
  const got = await sha256Hex(bytes);
  if (got !== expected) {
    throw new Error(`${label}: the download does not match its published SHA-256 (got ${got.slice(0, 12)}..., expected ${expected.slice(0, 12)}...)`);
  }
  return bytes;
}

function find(entries, pred, what) {
  const e = entries.find(pred);
  if (!e) throw new Error(`${what} is missing from the zip`);
  return e;
}

export async function buildCard({ manifest, strataVersion, includeFirmware, fetchAsset, onStep = () => {} }) {
  const strata = manifest.strata.find((s) => s.version === strataVersion);
  if (!strata) throw new Error(`STRATA ${strataVersion} is not in the list of releases`);
  const enaga = manifest.enaga[strata.enaga];
  if (!enaga) throw new Error(`Enaga ${strata.enaga} is not in the list of firmware releases`);
  const dec = new TextDecoder();
  const enc = new TextEncoder();

  onStep(`Downloading STRATA ${strata.version} from ${REPOS.strata}`);
  const strataZip = await verified(fetchAsset, assetId("strata", strata.version), strata.sha256, `STRATA ${strata.version}`);
  const sEntries = readZip(strataZip);
  const appEntry = find(sEntries, (e) => e.name.endsWith(APP_SUFFIX), "APP.ENA");
  const app = await appEntry.read();
  if (dec.decode(app.subarray(0, 4)) !== "ENGA") throw new Error("APP.ENA is not an Enaga package");

  const sumsEntry = sEntries.find((e) => e.name.endsWith("SHA256SUMS.txt"));
  if (sumsEntry) {
    const line = dec.decode(await sumsEntry.read()).split("\n").find((l) => l.trim().endsWith(APP_SUFFIX));
    if (!line) throw new Error("SHA256SUMS.txt does not list APP.ENA");
    if (line.trim().split(/\s+/)[0] !== (await sha256Hex(app))) throw new Error("APP.ENA does not match SHA256SUMS.txt");
  }
  const licence = await find(sEntries, (e) => e.name.endsWith("/LICENSE.txt"), "LICENSE.txt").read();
  const notices = await find(sEntries, (e) => e.name.endsWith("/THIRD_PARTY_NOTICES.md"), "THIRD_PARTY_NOTICES.md").read();

  onStep(`Downloading Enaga ${strata.enaga}'s licence from ${REPOS.enaga}`);
  const enagaLicence = await verified(fetchAsset, assetId("enaga", strata.enaga) + "-license", enaga.license_sha256, `Enaga ${strata.enaga}'s LICENSE`);
  if (!dec.decode(enagaLicence.subarray(0, 200)).includes("License")) throw new Error(`Enaga ${strata.enaga}'s LICENSE does not look like a licence`);

  let firmware = null;
  if (includeFirmware) {
    onStep(`Downloading Enaga ${strata.enaga} from ${REPOS.enaga}`);
    const enagaZip = await verified(fetchAsset, assetId("enaga", strata.enaga), enaga.sha256, `Enaga ${strata.enaga}`);
    const eEntries = readZip(enagaZip);
    firmware = await find(eEntries, (e) => e.name === "SmplTrek.bin", "SmplTrek.bin").read();
  }

  onStep("Building the card zip");
  const files = [];
  if (firmware) files.push({ name: "SmplTrek.bin", data: firmware });
  files.push({ name: CARD_APP, data: app });
  files.push({ name: "STRATA-README.txt", data: enc.encode(readme(strata, enaga, includeFirmware)) });
  files.push({ name: "ENAGA-LICENSE.txt", data: enagaLicence });
  files.push({ name: "STRATA-LICENSES.txt", data: enc.encode(licences(dec.decode(licence), dec.decode(notices))) });

  const name = includeFirmware
    ? `STRATA-v${strata.version}-enaga-v${strata.enaga}-sdcard.zip`
    : `STRATA-v${strata.version}-app-only-sdcard.zip`;
  return { name, zip: writeZip(files), files: files.map((f) => ({ name: f.name, size: f.data.length })) };
}

export function readme(strata, enaga, includeFirmware) {
  const v = strata.version;
  const e = strata.enaga;
  const lines = [
    `STRATA ${v} + Enaga ${e} for the Sonicware SmplTrek`,
    "=".repeat(`STRATA ${v} + Enaga ${e} for the Sonicware SmplTrek`.length),
    "",
    "Built in your browser from the two official releases:",
    `  STRATA ${v}: ${releaseUrl(REPOS.strata, strata.tag)}`,
    `  Enaga ${e}:  ${releaseUrl(REPOS.enaga, enaga.tag)}`,
    "Both zips were checked against their published SHA-256 before anything was taken from them.",
    "",
    "AT YOUR OWN RISK",
    "----------------",
    "Installing third-party firmware is at your own risk. Neither Enaga nor STRATA is made,",
    "supported or endorsed by Sonicware, and installing them may affect your warranty. Both are",
    "provided as is, without warranty of any kind (see ENAGA-LICENSE.txt and STRATA-LICENSES.txt).",
    "",
    "BEFORE YOU INSTALL",
    "------------------",
    "Do not skip these: a hung image leaves the power key dead.",
    "",
    "  [ ] SYSTEM INFO shows BOOT 1.0.17.",
    "  [ ] A second SD card holds an official SmplTrek.bin from Sonicware, so you can restore",
    "      the unit.",
    "  [ ] Holding HOME (F4) at power-on reaches BOOT MODE.",
    "  [ ] The unit runs on the power adapter WITHOUT batteries, so a hung image can be",
    "      power-cycled.",
    "",
    "WHAT IS IN THIS ZIP",
    "-------------------",
  ];
  if (includeFirmware) lines.push(`  SmplTrek.bin                 Enaga ${e}, the firmware (from Enaga's release, unchanged)`);
  lines.push(
    `  ${CARD_APP}    the STRATA ${v} app (from STRATA's release, unchanged)`,
    "  STRATA-README.txt            this file",
    "  ENAGA-LICENSE.txt            Enaga's licence (MIT), from its repository at that release",
    "  STRATA-LICENSES.txt          STRATA's licence and every third-party notice (Enaga, and the",
    "                               FatFs and TinyUSB code inside SmplTrek.bin)",
    "",
    "INSTALLING",
    "----------",
    "  1. Copy everything in this zip to the ROOT of the SD card, keeping the names and folders.",
    "     If the card already has an ENAGA folder, merge it and replace the old APP.ENA.",
  );
  if (includeFirmware) {
    lines.push(
      "  2. Put the card in the SmplTrek. Hold HOME (F4) while switching on to reach BOOT MODE,",
      `     and choose SYSTEM UPDATE. This installs Enaga ${e}. Let it finish.`,
      "  3. Switch on. Enaga starts the last app that ran, or the only one installed.",
    );
  } else {
    lines.push(
      `  2. This zip has no firmware: the unit must already run Enaga ${e} exactly. The Enaga menu`,
      "     (F1 + POWER) shows its version at the top.",
      "  3. Switch on. Enaga starts the last app that ran, or the only one installed.",
    );
  }
  lines.push(
    "     F1 + POWER opens the Enaga menu, which lists the apps and switches between them.",
    "",
    `STRATA ${v} needs Enaga ${e} exactly. Enaga's loader checks only an ABI number that several`,
    "Enaga versions share, so a mismatch starts and then misbehaves.",
    "",
    "Projects are saved on the card under /ENAGA/DATA/STRATA/ and stay there when you update.",
    "",
    "GOING BACK",
    "----------",
    "Install an official SmplTrek system update the same way (its SmplTrek.bin on the card's",
    "root, BOOT MODE, SYSTEM UPDATE). Neither Enaga nor STRATA writes the SPI flash or touches",
    "the bootloader, so that restores the unit completely.",
    "",
    "MORE",
    "----",
    `  Manual:     https://github.com/Devko/STRATA/blob/${strata.tag}/docs/MANUAL.md`,
    `  Cheatsheet: https://github.com/Devko/STRATA/blob/${strata.tag}/docs/CHEATSHEET.md`,
    `  Changelog:  https://github.com/Devko/STRATA/blob/${strata.tag}/CHANGELOG.md`,
    "",
  );
  return lines.join("\r\n");
}

function licences(licence, notices) {
  return [
    "STRATA's licence (LICENSE.txt from the STRATA release)",
    "======================================================",
    "",
    licence.trim(),
    "",
    "",
    "Third-party notices (THIRD_PARTY_NOTICES.md from the STRATA release). Its Enaga section is",
    "the licence of SmplTrek.bin as well.",
    "===========================================================================================",
    "",
    notices.trim(),
    "",
  ].join("\n");
}
