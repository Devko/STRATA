// The list of releases the card builder offers, read live from GitHub.
//
// A STRATA release is offered when it has its STRATA-vX.Y.Z-smpltrek.zip and says which Enaga it
// needs. tools/publish_release.sh writes that into the release body as an invisible comment,
//
//   <!-- strata-card: {"enaga":"0.3.0"} -->
//
// and releases from before that marker take it from public/compat.json. Each zip's SHA-256 is
// the digest GitHub publishes for the asset; for the releases pinned in compat.json the pinned
// hash wins, and a release whose GitHub digest disagrees with its pinned hash is not offered.
// If GitHub's API cannot be reached, the pinned list is served as it is.
//
// Unauthenticated API calls are limited to 60 an hour per IP, and the IPs a Pages Function
// calls out from are shared with every other Cloudflare site, so that budget is often spent
// before this site asks (2026-11-27: GitHub answered 403 and 0.5.0 was missing). So:
//   - answers are cached for ten minutes (Cloudflare's cache where there is one, else memory);
//   - the last good answer is also kept for a week, and served when GitHub refuses;
//   - only then is public/compat.json served as it is, so pin each release there too.
// A read-only GITHUB_TOKEN set as a Pages secret gives the site its own budget (5000 an hour)
// and is the real fix for the limit.

import pinned from "../public/compat.json" with { type: "json" };

export const REPOS = { strata: "Devko/STRATA", enaga: "seawaffle/enaga" };
const API = "https://api.github.com";
const TTL_S = 600;
const VERSION = /^\d+\.\d+\.\d+$/;
const MARKER = /<!--\s*strata-card:\s*(\{[^}]*\})\s*-->/;
const memory = new Map();

const STALE_S = 7 * 24 * 3600;

async function cachePut(cache, url, text, maxAge) {
  await cache.put(url, new Response(text, {
    headers: { "content-type": "application/json", "cache-control": `public, max-age=${maxAge}` },
  }));
}

// The last good answer for `url`, however old (up to STALE_S), or null.
async function lastGood(url, cache) {
  const hit = memory.get(url);
  if (hit) return hit.value;
  if (!cache) return null;
  const kept = await cache.match(`${url}#last-good`);
  return kept ? kept.json() : null;
}

async function getJson(url, env, fetchImpl) {
  const now = Date.now();
  const hit = memory.get(url);
  if (hit && hit.until > now) return hit.value;
  const cache = typeof caches !== "undefined" && caches.default ? caches.default : null;
  if (cache) {
    const cached = await cache.match(url);
    if (cached) return cached.json();
  }
  const headers = { "user-agent": "strata-card-builder", accept: "application/vnd.github+json" };
  if (env.GITHUB_TOKEN) headers.authorization = `Bearer ${env.GITHUB_TOKEN}`;
  let text;
  try {
    const r = await fetchImpl(url, { headers });
    if (!r.ok) {
      const left = r.headers.get("x-ratelimit-remaining");
      const limit = left === "0" ? " (rate limit spent)" : "";
      throw new Error(`GitHub API answered ${r.status}${limit} for ${url}`);
    }
    text = await r.text();
  } catch (err) {
    const stale = await lastGood(url, cache);
    if (stale) return stale;
    throw err;
  }
  const value = JSON.parse(text);
  memory.set(url, { value, until: now + TTL_S * 1000 });
  if (cache) {
    await cachePut(cache, url, text, TTL_S);
    await cachePut(cache, `${url}#last-good`, text, STALE_S);
  }
  return value;
}

function digestOf(asset) {
  const d = asset?.digest;
  return typeof d === "string" && /^sha256:[0-9a-f]{64}$/.test(d) ? d.slice(7) : null;
}

function key(v) {
  return v.split(".").map((n) => n.padStart(6, "0")).join(".");
}

export function enagaFromBody(body) {
  const m = MARKER.exec(body ?? "");
  if (!m) return null;
  try {
    const v = JSON.parse(m[1]).enaga;
    return typeof v === "string" && VERSION.test(v) ? v : null;
  } catch {
    return null;
  }
}

export async function liveManifest(env = {}, { fetchImpl = fetch } = {}) {
  let releases;
  try {
    releases = await getJson(`${API}/repos/${REPOS.strata}/releases?per_page=100`, env, fetchImpl);
  } catch (err) {
    return { ...pinned, source: "pinned", note: String(err.message ?? err) };
  }

  const pinnedStrata = new Map(pinned.strata.map((s) => [s.version, s]));
  const strata = [];
  for (const r of releases) {
    const m = /^v(\d+\.\d+\.\d+)$/.exec(r.tag_name ?? "");
    if (r.draft || !m) continue;
    const version = m[1];
    const asset = `STRATA-v${version}-smpltrek.zip`;
    const a = (r.assets ?? []).find((x) => x.name === asset);
    if (!a) continue;
    const pin = pinnedStrata.get(version);
    const digest = digestOf(a);
    if (pin && digest && pin.sha256 !== digest) continue; // the published file changed: refuse it
    const sha256 = pin?.sha256 ?? digest;
    const enaga = enagaFromBody(r.body) ?? pin?.enaga;
    if (!sha256 || !enaga) continue;
    strata.push({ version, tag: `v${version}`, date: (r.published_at ?? "").slice(0, 10) || pin?.date || "", asset, sha256, enaga });
  }

  const enaga = {};
  for (const v of new Set(strata.map((s) => s.enaga))) {
    if (pinned.enaga[v]) { enaga[v] = pinned.enaga[v]; continue; }
    try {
      const r = await getJson(`${API}/repos/${REPOS.enaga}/releases/tags/v${v}`, env, fetchImpl);
      const assetName = `enaga-v${v}-smpltrek.zip`;
      const sha256 = digestOf((r.assets ?? []).find((x) => x.name === assetName));
      if (!r.draft && sha256) enaga[v] = { tag: `v${v}`, asset: assetName, sha256 };
    } catch {
      // not offered: the STRATA releases that need it are dropped below
    }
  }

  const offered = strata.filter((s) => enaga[s.enaga]).sort((x, y) => (key(x.version) < key(y.version) ? 1 : -1));
  if (offered.length === 0) return { ...pinned, source: "pinned", note: "no release on GitHub could be offered" };
  return { schema: 1, source: "github", enaga, strata: offered };
}

// For the tests.
export function clearCache() {
  memory.clear();
}
