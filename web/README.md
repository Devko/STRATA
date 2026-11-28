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
  ...                          and any starter content picked (below)
  ```

- The asset function exists only because GitHub's release downloads send no CORS headers, so a
  page cannot read them directly. It streams one file from
  `https://github.com/<repo>/releases/download/<tag>/<asset>` (or Enaga's `LICENSE` from
  `raw.githubusercontent.com`) and stores nothing. It answers only for the releases that
  `/api/releases` offers, always from the two repositories, so it is not an open proxy, and the
  hash check in the browser means it could not slip in other bytes either.
- GitHub's API allows 60 unauthenticated calls an hour per IP, and the IPs a Pages Function
  calls out from are shared with every other Cloudflare site, so that budget is often spent
  before this site asks: on 2026-11-27 GitHub answered 403 and the site offered only
  `compat.json`, without the new 0.5.0. Answers are cached for ten minutes, the last good answer
  is kept for a week (Cloudflare's cache, per data centre) and served when GitHub refuses, and
  only then does the page fall back to `compat.json`. A read-only `GITHUB_TOKEN` set as a Pages
  secret (`npx wrangler pages secret put GITHUB_TOKEN`, a fine-grained token with no
  permissions beyond public repositories) gives the site its own 5000 calls an hour and is the
  real fix; it takes effect with the next deploy.

### Licence

Enaga is MIT; its firmware also contains FatFs (1-clause BSD-style) and TinyUSB (MIT). MIT
permits redistribution, including of the binary, if the notice goes with it, so passing the
official file through is fine, and the zip carries Enaga's licence as `ENAGA-LICENSE.txt` and every
notice in `STRATA-LICENSES.txt`.
STRATA's own releases still do not bundle Enaga (decided 2026-11-25): the user's browser fetches
it from Enaga's release at the moment they ask, so users always get the authors' file, checked
against its hash.

## Starter content

The page can add three optional things to the card, each a tick box (section 2 of the page):

```text
ENAGA/DATA/STRATA/PROJECTS/<NAME>/   starter projects, 11 files each (STRATA 0.5.0 and newer)
SAMPLES/STRATA/<FOLDER>/*.WAV        the sample pack: DRUMS, PERC, BASS, LEADS, CHORDS
STRATA-QUICKSTART.txt                one page: the first things to try
```

They are served from this site, `public/starter/`, and listed in `public/starter/starter.json`
with each file's SHA-256, which the browser checks as it does the release zips. Projects are
copied into the card zip still deflated, so ten projects of mostly empty banks add about 70 KB.
The projects and the quick start are offered only for STRATA `min_strata` (0.5.0) and newer:
they are saved in file format C, and 0.4.0 reads their LFOs wrong. The pack is plain WAV.

**Where it comes from.** Nothing in `public/starter/` is edited by hand:

- `content/starter/projects/<NAME>/` holds each project's files as STRATA saves them, and
  `content/starter/projects.json` the line the page shows for it. `tests/test_starter.cpp` opens
  every folder there through `app/storage.cpp` on each CI run, so a format change that would stop
  them opening fails there first. `tools/showcases/build.sh --starter` writes the nine
  showcases there from their generators. TECHNO, which has no generator in the repository, was
  opened and saved again through the app's real Storage by `build/host/strata-starter resave SRC
  NAME OUT` (`make -C host starter`, `host/starter.cpp`), which brings any older project file up
  to what this build writes; v0.5.0's own code reads the re-saved file exactly as the original.
- The sample pack is rendered by `build/host/strata-starter samples DIR`: every factory sound of
  KICK, SNARE, HAT, PERC, VA MONO, WAVE MONO, VA POLY, WAVE POLY and FM4 through the real engine,
  dry, 48 kHz 16-bit mono, normalised to -1 dBFS, at most 3 s. Melodic files carry their note in
  the name (`MONO_ACID_C2`), and a SAMPLE track playing C4 plays a file at its own pitch; chords
  are C minor sevenths (`_C3M7`).
- `content/starter/QUICKSTART.txt` is the quick start.
- `node tools/starter.mjs [--samples DIR]` packs all of it into `public/starter/` and writes
  `starter.json`; without `--samples` it keeps the pack that is there. `npm test` checks that
  `starter.json` matches the files and that every project zip holds exactly what
  `content/starter/projects/` holds, so a stale pack fails CI.

To change a project: regenerate or re-save it into `content/starter/projects/<NAME>/`, run
`make -C host test` and `node tools/starter.mjs`, commit both, and deploy. To add one, also give
it a line in `projects.json`. To re-render the pack after a sound changes:

```sh
make -C host starter && build/host/strata-starter samples /tmp/pack   # from the repository root
cd web && node tools/starter.mjs --samples /tmp/pack
```

## Adding a release

A release published with `tools/publish_release.sh` carries the marker and appears within ten
minutes, with no deploy, whenever GitHub's API answers. A release made by hand needs the marker
line in its body. Pin each release in `compat.json` as well, and deploy, so that it is offered
even when GitHub refuses the site; `npm test` checks the file.

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
