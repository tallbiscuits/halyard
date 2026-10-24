#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Mainsail for Halyard (D13): upstream Mainsail at the tag in UPSTREAM, plus patches/*.patch.
# Idempotent. Node runs in Docker; nothing is installed on the PC.
#   ./run.sh build          the upstream tag + the patches, built into
#                           dist/mainsail-<tag>-halyard.<REVISION>.zip
#   ./run.sh src            work/src: a checkout of the tag with the patches applied as
#                           commits, on branch "halyard", to edit (commit each change there)
#   ./run.sh export         work/src's commits after the tag, written back to patches/
#   ./run.sh serve [host]   build, then serve it on http://localhost:8080 through the API-capture
#                           proxy, talking to Halyard at host (default halyard.local)
# A new upstream tag: change UPSTREAM, set REVISION to 1, "src" (fix any patch that no longer
# applies, in work/src), "export", then run an API capture (tools/api-capture) and the tests (D13).
set -e
cd "$(dirname "$0")"
UPSTREAM=$(cat UPSTREAM)
VERSION=$UPSTREAM-halyard.$(cat REVISION)
SRC=work/src
ZIP=dist/mainsail-$VERSION.zip
REPO=https://github.com/mainsail-crew/mainsail.git
NODE_IMAGE=node:22.12-bookworm-slim  # Mainsail 2.19 wants Node ^20.19 or >=22.12

# work/src at the tag, the patches applied as commits on "halyard". Local commits that were
# not exported are kept on a backup branch rather than lost.
prepare_src() {
  if [ ! -d "$SRC/.git" ]; then
    mkdir -p work
    git clone -q --filter=blob:none --no-checkout "$REPO" "$SRC"
  fi
  git -C "$SRC" rev-parse -q --verify "refs/tags/$UPSTREAM" >/dev/null \
    || git -C "$SRC" fetch -q origin "refs/tags/$UPSTREAM:refs/tags/$UPSTREAM"
  if git -C "$SRC" rev-parse -q --verify halyard >/dev/null; then
    git -C "$SRC" branch -f "halyard-backup-$(date +%Y%m%d-%H%M%S)" halyard
  fi
  git -C "$SRC" am --abort 2>/dev/null || true
  git -C "$SRC" checkout -q -f -B halyard "$UPSTREAM"
  git -C "$SRC" clean -qfdx -e node_modules
  local p
  for p in patches/*.patch; do
    [ -e "$p" ] || continue
    git -C "$SRC" -c user.name=Halyard -c user.email=halyard@invalid am -q --committer-date-is-author-date "$PWD/$p" \
      || { echo "$p no longer applies: fix it in $SRC (git am --continue), then ./run.sh export" >&2; exit 1; }
  done
}

# Halyard's config reference, from the card's halyard.cfg (one source for both).
reference() { python3 config_reference.py ../card/halyard.cfg "$SRC/public/halyard-config.html"; }

build() {
  prepare_src
  reference
  mkdir -p work/npm-cache dist
  # The version goes into package.json only for the build (Mainsail writes .version and
  # release_info.json from it); the checkout stays as the patches left it.
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e npm_config_cache=/cache \
    -v "$PWD/$SRC:/src" -v "$PWD/work/npm-cache:/cache" -w /src "$NODE_IMAGE" sh -c "
      set -e
      npm ci --no-audit --no-fund --loglevel=error
      npm pkg set version=${VERSION#v}
      npx vite build --logLevel warn"
  git -C "$SRC" checkout -q -- package.json
  # Like Mainsail's own "zip -r": a file is deflated only if that makes it smaller. Halyard
  # turns each deflated entry into a .gz file as it installs (ZipToTar), so a tiny .version
  # must stay stored, or Halyard can't read it.
  python3 - "$SRC/dist" "$ZIP" <<'PY'
import os, sys, zipfile, zlib
src, out = sys.argv[1:]
with zipfile.ZipFile(out, "w") as z:
    for root, _, names in os.walk(src):
        for n in sorted(names):
            p = os.path.join(root, n)
            data = open(p, "rb").read()
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            smaller = len(c.compress(data) + c.flush()) < len(data)
            z.write(p, os.path.relpath(p, src), zipfile.ZIP_DEFLATED if smaller else zipfile.ZIP_STORED,
                    compresslevel=9 if smaller else None)
PY
  echo "$ZIP: $(cat "$SRC/dist/.version"), $(find "$SRC/dist" -type f | wc -l) files, $(du -h "$ZIP" | cut -f1)"
}

case "${1:-build}" in
  build)  build ;;
  src)    prepare_src; echo "$SRC: $UPSTREAM + $(ls patches/*.patch 2>/dev/null | wc -l) patches on branch halyard" ;;
  export) [ -d "$SRC/.git" ] || { echo "no $SRC: run ./run.sh src first" >&2; exit 1; }
          rm -f patches/*.patch
          git -C "$SRC" format-patch -q --zero-commit --no-signature --no-stat -o "$PWD/patches" "$UPSTREAM..halyard"
          ls patches ;;
  serve)  HOST="${2:-halyard.local}"; build
          rm -rf work/www && mkdir -p work/www && (cd work/www && unzip -q "../../$ZIP")
          exec ../tools/api-capture/run.sh proxy --upstream "http://$HOST" --www "$PWD/work/www" \
            --log "$PWD/work/serve-api.jsonl" ;;
  *)      echo "usage: $0 [build | src | export | serve [host]]" >&2; exit 2 ;;
esac
