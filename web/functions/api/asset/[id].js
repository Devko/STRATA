// Cloudflare Pages Function: GET /api/asset/<id>, e.g. /api/asset/strata-0.4.0 or enaga-0.3.0.
//
// GitHub's release downloads send no CORS headers, so a page cannot fetch them itself. This
// passes one official release file through, as it streams from GitHub: nothing is stored,
// rewritten or cached here beyond the browser's own cache. It serves only the files of the
// releases that /api/releases offers (lib/releases.js), always from the two STRATA and Enaga
// repositories, so it is not an open proxy, and the page checks every zip against its SHA-256
// before using it.

import { liveManifest } from "../../../lib/releases.js";
import { allowedAssets } from "../../../public/card.js";

export async function onRequestGet({ params, env }) {
  const allowed = allowedAssets(await liveManifest(env ?? {}));
  const a = Object.hasOwn(allowed, params.id) ? allowed[params.id] : null;
  if (!a) return new Response("unknown file\n", { status: 404, headers: { "content-type": "text/plain" } });

  const url = a.url;
  const upstream = await fetch(url, { redirect: "follow", headers: { "user-agent": "strata-card-builder" } });
  if (!upstream.ok) {
    return new Response(`GitHub answered ${upstream.status} for ${a.name}\n`, {
      status: 502,
      headers: { "content-type": "text/plain" },
    });
  }

  const headers = new Headers({
    "content-type": a.name.endsWith(".zip") ? "application/zip" : "text/plain; charset=utf-8",
    // The bytes behind an id never change: compat.json pins their hash.
    "cache-control": "public, max-age=86400, immutable",
    "x-source-url": url,
  });
  // fetch() decodes a gzip'd body (raw.githubusercontent.com sends one) but keeps the encoded
  // length, so pass the length on only for a body that was not encoded.
  const length = upstream.headers.get("content-length");
  if (length && !upstream.headers.get("content-encoding")) headers.set("content-length", length);
  return new Response(upstream.body, { status: 200, headers });
}
