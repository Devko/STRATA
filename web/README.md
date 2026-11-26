# STRATA card builder

A small website that builds a ready-to-copy SD card zip for the SmplTrek: the STRATA app from
[STRATA's releases](https://github.com/Devko/STRATA/releases) and the Enaga firmware it needs from
[Enaga's releases](https://github.com/seawaffle/enaga/releases), in versions that belong together.
It runs on Cloudflare Pages, with one Pages Function.

## How it works

- `public/compat.json` lists every STRATA release, the Enaga version it needs, and the SHA-256 of
  each release zip. It is the only place the pairs are decided. The page offers only these pairs.
- The browser downloads the two official zips through `functions/api/asset/[id].js`, checks each
  against its SHA-256 in `compat.json`, unzips them (`public/zip.js`, the browser's own
  `DecompressionStream`), checks `APP.ENA` against the release's `SHA256SUMS.txt` and Enaga's
  package magic, and writes a new zip (`public/card.js`):

  ```text
  SmplTrek.bin                 Enaga's firmware, unchanged (left out for "STRATA only")
  ENAGA/APPS/STRATA/APP.ENA    the STRATA app, unchanged
  STRATA-README.txt            install steps for this pair
  STRATA-LICENSES.txt          STRATA's LICENSE.txt and THIRD_PARTY_NOTICES.md (Enaga's MIT
                               notice, FatFs, TinyUSB)
  ```

- The function exists only because GitHub's release downloads send no CORS headers, so a page
  cannot read them directly. It streams one file from
  `https://github.com/<repo>/releases/download/<tag>/<asset>` and stores nothing. It answers
  only for ids built from `compat.json` (`strata-0.4.0`, `enaga-0.3.0`, ...), so it is not an
  open proxy, and the hash check in the browser means it could not slip in other bytes either.

### Licence

Enaga is MIT; its firmware also contains FatFs (1-clause BSD-style) and TinyUSB (MIT). MIT
permits redistribution, including of the binary, if the notice goes with it, so passing the
official file through is fine, and the zip carries every notice in `STRATA-LICENSES.txt`.
STRATA's own releases still do not bundle Enaga (decided 2026-11-25): the user's browser fetches
it from Enaga's release at the moment they ask, so users always get the authors' file, checked
against a pinned hash.

## Adding a release

[docs/release/RELEASING.md](../docs/release/RELEASING.md), "After": add the release as the first
entry of `public/compat.json`, then `npm test`. The tests check the list (newest first, every
STRATA names a listed Enaga, the newest one needs the Enaga that `third_party/enaga` pins).

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

Either connect the repository, which redeploys on every push to `main`:

1. Cloudflare dashboard, **Workers & Pages**, **Create**, **Pages**, **Connect to Git**, pick
   `Devko/STRATA`.
2. Production branch `main`. Framework preset **None**. Build command: empty. Build output
   directory: `public`. **Root directory: `web`**. (The function in `web/functions/` is found
   automatically.)
3. After the first deploy: the project's **Custom domains**, add the subdomain (for example
   `strata.einblick.dev`). With `einblick.dev` on Cloudflare DNS the record is created for you.

Or deploy from a computer with wrangler, after `npx wrangler login` once:

```sh
cd web
npx wrangler pages deploy        # reads wrangler.toml: project strata-card, output public/
```

No secrets or environment variables are needed: everything it downloads is public.
