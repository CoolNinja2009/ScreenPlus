#!/bin/bash
# Screen+ for macOS (Apple silicon, macOS 12+): builds it and packs build-mac/Screen+.app and
# build-mac/ScreenPlus-<version>-macos.dmg. Needs Xcode's command line tools (xcode-select --install) and Homebrew.
#
#   scripts/build-mac.sh [version]          build, pack, and say how to try it
#   scripts/build-mac.sh --run [version]    the same, then start it from the build and collect a report
#
# The window is the Bare one (SDL3 GPU on Metal); the classic window isn't built on macOS.
# Homebrew's libraries (SDL3, FFmpeg) are copied into the app (dylibbundler), so the .dmg runs on a Mac
# without Homebrew. The app is signed ad hoc only: on another Mac, right-click > Open the first time.
set -euo pipefail
cd "$(dirname "$0")/.."

RUN=0
if [ "${1:-}" = "--run" ]; then RUN=1; shift; fi
VERSION=${1:-$(sed -n 's/^set(SCREENPLUS_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt)}
BUILD=build-mac
APP="$BUILD/Screen+.app"

if [ "$(uname)" != Darwin ]; then echo "This builds the macOS app; run it on a Mac." >&2; exit 1; fi
xcode-select -p >/dev/null 2>&1 || { echo "Install Xcode's command line tools first: xcode-select --install" >&2; exit 1; }
if ! command -v brew >/dev/null; then
    echo "Install Homebrew first: https://brew.sh" >&2
    exit 1
fi
if [ "${SKIP_BREW:-0}" != 1 ]; then
    brew install --quiet cmake ninja pkg-config sdl3 ffmpeg dylibbundler
fi

# Bare: a Bare folder beside this one (the zip a tester gets), else the checkout beside Wallpaper+, else the pinned commit
BARE=()     # used as ${BARE[@]+...}: bash 3.2 calls an empty array unbound
if [ -f ../Bare/CMakeLists.txt ]; then BARE=(-DBARE_SOURCE_DIR="$(cd ../Bare && pwd)"); fi
cmake -S . -B "$BUILD" -G Ninja ${BARE[@]+"${BARE[@]}"} -DCMAKE_BUILD_TYPE=Release -DSCREENPLUS_VERSION="$VERSION" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_OSX_ARCHITECTURES=arm64 -DSCREENPLUS_BARE_UI=ON -DSCREENPLUS_UI=OFF
cmake --build "$BUILD"

OUT="$BUILD/src/linux"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
# The programs and Homebrew's libraries are bundled and signed outside the app: codesign takes a program in
# Contents/MacOS as the whole bundle's, and the samples and fonts beside it can't be signed
PRE="$BUILD/pre"
rm -rf "$PRE"
mkdir -p "$PRE/MacOS" "$PRE/Frameworks"
for f in screenplus screenplus-engine screenplus-bare-ui screenplus-depth.so; do
    if [ -f "$OUT/$f" ]; then cp "$OUT/$f" "$PRE/MacOS/"; else echo "note: $f was not built"; fi
done
[ -f "$PRE/MacOS/screenplus-bare-ui" ] || { echo "The Bare window didn't build (it needs SDL3 3.4+)" >&2; exit 1; }
cp resources/depth.html resources/splat.html "$APP/Contents/MacOS/"
cp -R resources/fonts samples "$APP/Contents/MacOS/"
cp src/linux/kde/org.screenplus.wallpaper/contents/ui/pointer.js "$APP/Contents/Resources/"
sed "s/@VERSION@/$VERSION/g" src/mac/Info.plist > "$APP/Contents/Info.plist"

# Icon: Quick Look renders the SVG, sips and iconutil make the .icns
ICON=$(mktemp -d)
if qlmanage -t -s 1024 -o "$ICON" branding/icon.svg >/dev/null 2>&1 && [ -f "$ICON/icon.svg.png" ]; then
    mkdir "$ICON/ScreenPlus.iconset"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$ICON/icon.svg.png" --out "$ICON/ScreenPlus.iconset/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$ICON/icon.svg.png" --out "$ICON/ScreenPlus.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICON/ScreenPlus.iconset" -o "$APP/Contents/Resources/ScreenPlus.icns" || echo "note: no icon"
fi
rm -rf "$ICON"

# Homebrew's libraries into Contents/Frameworks; dylibbundler signs each program and library (ad hoc)
BINS=()
for f in "$PRE"/MacOS/screenplus*; do BINS+=(-x "$f"); done
dylibbundler -of -cd -b "${BINS[@]}" -d "$PRE/Frameworks" -p @executable_path/../Frameworks/ >/dev/null
for f in "$PRE"/MacOS/* "$PRE"/Frameworks/*; do codesign --force --sign - "$f"; done
cp "$PRE"/MacOS/* "$APP/Contents/MacOS/"
cp "$PRE"/Frameworks/* "$APP/Contents/Frameworks/"
DMG="$BUILD/ScreenPlus-$VERSION-macos.dmg"
rm -f "$DMG"
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "Screen+" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"
echo
echo "Built $APP and $DMG"

if [ $RUN = 0 ]; then
    echo "Try it:  open \"$APP\"        (or: \"$APP/Contents/MacOS/screenplus\" help)"
    echo "Then send back the report: scripts/mac-report.sh"
    exit 0
fi
"$APP/Contents/MacOS/screenplus" stop >/dev/null 2>&1 || true
open "$APP"
sleep 8
exec scripts/mac-report.sh
