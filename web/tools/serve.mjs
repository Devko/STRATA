// A stand-in for Cloudflare Pages, for local testing without wrangler:
// serves public/ and routes /api/releases and /api/asset/<id> to the Pages Functions.
//
//   NODE_USE_ENV_PROXY=1 node web/tools/serve.mjs [port]   (the env var only behind a proxy)

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { Readable } from "node:stream";
import { onRequestGet } from "../functions/api/asset/[id].js";
import { onRequestGet as releases } from "../functions/api/releases.js";

const ROOT = fileURLToPath(new URL("../public/", import.meta.url));
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".txt": "text/plain; charset=utf-8" };

export function startServer(port = 8788) {
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, "http://localhost");
    try {
      const m = url.pathname.match(/^\/api\/asset\/([^/]+)$/);
      if (m || url.pathname === "/api/releases") {
        const env = {};
        const r = m ? await onRequestGet({ params: { id: decodeURIComponent(m[1]) }, env }) : await releases({ env });
        res.writeHead(r.status, Object.fromEntries(r.headers));
        if (r.body) Readable.fromWeb(r.body).pipe(res); else res.end();
        return;
      }
      const path = normalize(join(ROOT, url.pathname === "/" ? "index.html" : url.pathname));
      if (!path.startsWith(ROOT)) throw new Error("outside");
      const body = await readFile(path);
      res.writeHead(200, { "content-type": TYPES[extname(path)] ?? "application/octet-stream" });
      res.end(body);
    } catch {
      res.writeHead(404).end("not found\n");
    }
  });
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number(process.argv[2] ?? 8788);
  await startServer(port);
  console.log(`http://localhost:${port}/`);
}
