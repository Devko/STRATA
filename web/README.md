# STRATA card builder

A small website that builds a ready-to-copy SD card zip for the SmplTrek: the STRATA app from
[STRATA's releases](https://github.com/Devko/STRATA/releases) and the Enaga firmware it needs from
[Enaga's releases](https://github.com/seawaffle/enaga/releases), in versions that belong together.
It runs on Cloudflare Pages at [strata.einblick.dev](https://strata.einblick.dev), with two Pages
Functions.

## How it works

- `/api/releases` (`functions/api/releases.js`, `lib/releases.js`) lists the releases on offer,
  read live from GitHub and cached for ten minutes. A STRATA release is offered when it has its
  `STRATA-vX.Y.Z-smpltrek.zip` and its body names the Enaga it needs in an invisible comment,
  `<!-- strata-card: {"enaga":"0.3.0"} -->`, which `tools/publish_release.sh` adds. Each zip's
  SHA-256 is the digest GitHub publishes for it, and the Enaga zip's comes from Enaga's release
  the same way. A release with no marker, a missing Enaga or no digest is not offered.
- `public/compat.json` pins the releases from before the marker (0.1.0 to 0.4.0, Enaga 0.2.0 and
  0.3.0). For those, the pinned Enaga version and hashes win, and a release whose GitHub digest
  stops matching its pin is refused. When GitHub's API cannot be reached, the page offers this
  list as it is.
- The browser downloads the official files through `functions/api/asset/[id].js`, checks each zip
  against its SHA-256, unzips them (`public/zip.js`, the browser's own `DecompressionStream`),
  checks `APP.ENA` against the release's `SHA256SUMS.txt` and Enaga's package magic, and writes
  a new zip (`public/card.js`):

  ```text
  SmplTrek.bin                 Enaga's firmware, unchanged (left out for "STRATA only")
  ENAGA/APPS/STRATA/APP.ENA    the STRATA app, unchanged
  STRATA-README.txt            install steps for this pair
  ENAGA-LICENSE.txt            Enaga's LICENSE from its repository at the release tag
  STRATA-LICENSES.txt          STRATA's LICENSE.txt and THIRD_PARTY_NOTICES.md (Enaga's MIT
                               notice, FatFs, TinyUSB)
  ```

- The asset function exists only because GitHub's release downloads send no CORS headers, so a
  page cannot read them directly. It streams one file from
  `https://github.com/<repo>/releases/download/<tag>/<asset>` (or Enaga's `LICENSE` from
  `raw.githubusercontent.com`) and stores nothing. It answers only for the releases that
  `/api/releases` offers, always from the two repositories, so it is not an open proxy, and the
  hash check in the browser means it could not slip in other bytes either.
- GitHub's API allows 60 unauthenticated calls an hour per IP, and Cloudflare's IPs are shared.
  The ten-minute cache keeps the site far below that; should it still hit the limit, the page
  falls back to `compat.json`. A read-only `GITHUB_TOKEN` set as a Pages secret
  (`npx wrangler pages secret put GITHUB_TOKEN`) lifts the limit, but nothing needs one.

### Licence

Enaga is MIT; its firmware also contains FatFs (1-clause BSD-style) and TinyUSB (MIT). MIT
permits redistribution, including of the binary, if the notice goes with it, so passing the
official file through is fine, and the zip carries Enaga's licence as `ENAGA-LICENSE.txt` and every
notice in `STRATA-LICENSES.txt`.
STRATA's own releases still do not bundle Enaga (decided 2026-11-25): the user's browser fetches
it from Enaga's release at the moment they ask, so users always get the authors' file, checked
against its hash.

## Adding a release

Nothing to do here: a release published with `tools/publish_release.sh` carries the marker and
appears within ten minutes, with no deploy. A release made by hand needs the marker line in its
body. `compat.json` changes only to pin something on purpose; `npm test` checks it.

## Running it locally

Node 22 or newer, no `npm install`:

```sh
cd web
npm test                       # offline unit tests
node tools/serve.mjs           # http://localhost:8788/, static files + the function
```

Behind an HTTP proxy, run the server with `NODE_USE_ENV_PROXY=1`. `wrangler pages dev` works
too, if wrangler is installed.

## Deploying (Cloudflare Pages)

The site is deployed by hand with wrangler from Roland's PC. The repository is deliberately not
connected to Cloudflare (decided 2026-11-26), so a merge does not redeploy: deploy again after a
change under `web/` reaches `main`. New releases need no deploy.

```sh
cd web
npx wrangler login               # once per machine, approved in the browser
npx wrangler pages deploy        # reads wrangler.toml: project strata-card, output public/
```

Run it from `web/` so that `functions/` is bundled with the page. The first deploy needs the
project: `npx wrangler pages project create strata-card --production-branch main`. The custom
domain is added once in the dashboard: Workers & Pages, `strata-card`, Custom domains (wrangler
has no command for it). No secrets or environment variables are needed: everything the site
downloads is public.
