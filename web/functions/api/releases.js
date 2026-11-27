// Cloudflare Pages Function: GET /api/releases, the STRATA releases the card builder offers,
// each with the Enaga version it needs and the SHA-256 of every zip. See lib/releases.js.

import { liveManifest } from "../../lib/releases.js";

export async function onRequestGet({ env }) {
  const manifest = await liveManifest(env ?? {});
  return new Response(JSON.stringify(manifest), {
    headers: { "content-type": "application/json", "cache-control": "public, max-age=300" },
  });
}
