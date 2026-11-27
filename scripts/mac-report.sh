#!/bin/bash
# What a macOS tester sends back: the Mac, the build, Screen+'s state and logs, and any crash reports, in one zip on
# the Desktop. Nothing personal beyond file names in the library; --screenshot adds a picture of the screens.
set -u
cd "$(dirname "$0")/.."
APP="${SCREENPLUS_APP:-build-mac/Screen+.app}"
[ -d "$APP" ] || APP="/Applications/Screen+.app"
SP="$APP/Contents/MacOS/screenplus"
DATA="${SCREENPLUS_DATA_DIR:-$HOME/Library/Application Support/ScreenPlus}"
OUT=$(mktemp -d)/screenplus-report
mkdir -p "$OUT"

{
    echo "== system"; sw_vers; uname -m; sysctl -n machdep.cpu.brand_string
    echo "== displays"; system_profiler SPDisplaysDataType 2>/dev/null | grep -E "Resolution|Display Type|Main Display|:$" | head -40
    echo "== build"; git log -1 --format="%h %s" 2>/dev/null; "$SP" version 2>&1
    echo "== processes"; ps -axo pid,rss,%cpu,command | grep -i "[s]creenplus"
    echo "== status"; "$SP" status 2>&1
    echo "== settings"; "$SP" settings 2>&1 | head -80
    echo "== libraries"; otool -L "$APP"/Contents/MacOS/screenplus* 2>&1 | grep -v "/System/\|/usr/lib/"
} > "$OUT/report.txt" 2>&1
"$SP" status --json > "$OUT/status.json" 2>/dev/null
cp "$DATA"/logs/*.log "$OUT/" 2>/dev/null
cp "$DATA/config.ini" "$OUT/" 2>/dev/null
find "$HOME/Library/Logs/DiagnosticReports" -iname "*screenplus*" -mtime -2 -exec cp {} "$OUT/" \; 2>/dev/null
if [ "${1:-}" = "--screenshot" ]; then screencapture -x "$OUT/screens.png"; fi

ZIP="$HOME/Desktop/screenplus-report-$(date +%Y%m%d-%H%M).zip"
(cd "$(dirname "$OUT")" && zip -qr "$ZIP" screenplus-report)
echo "Report: $ZIP"
