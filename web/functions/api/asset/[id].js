// Cloudflare Pages Function: GET /api/asset/<id>, e.g. /api/asset/strata-0.4.0 or enaga-0.3.0.
//
// GitHub's release downloads send no CORS headers, so a page cannot fetch them itself. This
// passes one official release zip through, as it streams from github.com: nothing is stored,
// rewritten or cached here beyond the browser's own cache. It serves only the files listed in
// public/compat.json, so it is not an open proxy, and the page checks every byte against the
// SHA-256 listed there before using it.

import manifest from "../../../public/compat.json" with { type: "json" };
import { allowedAssets } from "../../../public/card.js";

const ALLOWED = allowedAssets(manifest);

export async function onRequestGet({ params }) {
  const a = Object.hasOwn(ALLOWED, params.id) ? ALLOWED[params.id] : null;
  if (!a) return new Response("unknown file\n", { status: 404, headers: { "content-type": "text/plain" } });

  const url = `https://github.com/${a.repo}/releases/download/${a.tag}/${a.asset}`;
  const upstream = await fetch(url, { redirect: "follow", headers: { "user-agent": "strata-card-builder" } });
  if (!upstream.ok) {
    return new Response(`GitHub answered ${upstream.status} for ${a.asset}\n`, {
      status: 502,
      headers: { "content-type": "text/plain" },
    });
  }

  const headers = new Headers({
    "content-type": "application/zip",
    // The bytes behind an id never change: compat.json pins their hash.
    "cache-control": "public, max-age=86400, immutable",
    "x-source-url": url,
  });
  const length = upstream.headers.get("content-length");
  if (length) headers.set("content-length", length);
  return new Response(upstream.body, { status: 200, headers });
}
