#!/usr/bin/env bash
# Copies the .spx scanner from the Screen- repository (default ../ScreenMinus) into third_party/spxscan.
# The scanner is developed there; never edit the copy here.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=${1:-$here/../ScreenMinus}
dst=$here/third_party/spxscan
mkdir -p "$dst/third_party"
for f in spx_scan.h scan_int.h zip.h zip.c pck.c manifest.c rules.c sjson.c sjson.h extract.c scan.c; do
	cp "$src/scanner/$f" "$dst/$f"
done
cp "$src/scanner/third_party/puff.c" "$src/scanner/third_party/puff.h" "$dst/third_party/"
git -C "$src" rev-parse --short HEAD > "$dst/SCREENMINUS_COMMIT"
echo "spxscan synced from $(cat "$dst/SCREENMINUS_COMMIT")"
