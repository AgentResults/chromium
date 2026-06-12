# Aurelian / Asmodeus Chromium fork — base & reproducibility

This repository (`agentresults/chromium`, branch `asmodeus`) is a **thin overlay**
on Chromium, NOT a full Chromium clone. It tracks only the fork`s own additions and
modifications (the `aurelian/` embodiment, the `chrome/browser/asmodeus/` + `chrome/
browser/aurelian/` hooks, the vendored `velite/` wire sources, and the touched
build files) — currently ~327 files / on the order of 100+ commits, all pushed.

## Why the Chromium source is not committed here

The working Chromium checkout is ~54 GB (millions of files, Google`s full history).
GitHub rejects files > 100 MB and is unusable for multi-GB trees — this is exactly
why Chromium lives on Gerrit/googlesource and is *fetched* with `depot_tools`,
never committed. So the overlay is what lives in git; the base is pinned below.

## Base pin

- Chromium version: **148.0.7765.0**  (`chrome/VERSION`)
- Upstream: `https://chromium.googlesource.com/chromium/src.git`
- gclient solution: see `build-meta/gclient.config`\n- exact base reconstruction: `build-meta/base-state-vs-148.0.7765.0.patch` (202 stock files; the precise src SHA was managed:False / unrecorded, so the base state is captured as a patch against the tag) + the pinned `DEPS`

## Reproduce from scratch

```bash
# 1. depot_tools on PATH
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=$PWD/depot_tools:$PATH

# 2. fetch Chromium, pin to the base version
mkdir chromium && cd chromium
cp /path/to/build-meta/gclient.config .gclient
gclient sync --revision src@refs/tags/148.0.7765.0 --no-history

# 2b. reconstruct the EXACT base source state (the checkout was managed:False and
#     drifted from the tag; this patch + the pinned DEPS make it bit-exact)
cd src && git apply ../path/to/build-meta/base-state-vs-148.0.7765.0.patch && cd ..

# 3. overlay this repo on top of src/ (its tracked files replace/add into the tree)
cd src && git init && git remote add agentresults <fork-url> && git fetch agentresults asmodeus && git checkout asmodeus

# 4. build (Apple Silicon: Rosetta on; depot_tools on PATH)
PATH=\$HOME/depot_tools:\$PATH autoninja -C out/Default aurelian_browsertests aurelian_unittests chrome
```

## Fork remote

`agentresults` → `github.com/AgentResults/chromium` (branch `asmodeus`).
