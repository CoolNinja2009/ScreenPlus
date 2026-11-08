#!/usr/bin/env bash
# Builds SDL3 and gtk4-layer-shell as shared libraries for the Ubuntu .deb, which is built on Ubuntu 24.04 (KDE neon's
# base) so it installs there and on every newer Ubuntu: 24.04 has neither library. They ship beside the program in
# /usr/lib/screenplus (screenplus-ui.so's and screenplus-engine's RUNPATH is $ORIGIN). SDL3 loads Wayland and X11
# itself at run time, so only their headers are needed here.
#
# Needs cmake, ninja, meson, gcc, pkg-config and the GTK 4, Wayland and X11 headers. Usage: scripts/build-libs-linux.sh <prefix>
# then configure CMake with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig and ship <prefix>/lib/*.so.*.
set -euo pipefail

SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
GLS_VERSION=1.0.4
GLS_SHA256=7fe327dc3740e4b6f5edfd855e23f84b1ac1ec6854b731047b95df7feb46498b
PREFIX=$(realpath -m "${1:?prefix}")
WORK=$PREFIX.src
mkdir -p "$WORK"

fetch() {   # url file sha256
    [ -f "$2" ] || curl -L --fail --retry 5 --retry-all-errors --retry-delay 15 -o "$2" "$1"
    echo "$3  $2" | sha256sum -c -
    tar -xzf "$2" -C "$WORK"
}

fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" "$WORK/sdl.tar.gz" "$SDL_SHA256"
cmake -S "$WORK/SDL3-$SDL_VERSION" -B "$WORK/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_WAYLAND=ON -DSDL_X11=ON
cmake --build "$WORK/sdl-build"
cmake --install "$WORK/sdl-build"
# The window needs both video drivers; a missing header would quietly drop one
grep -qaF 'libwayland-client.so.0' "$PREFIX/lib/libSDL3.so.0"
grep -qaF 'libX11.so.6' "$PREFIX/lib/libSDL3.so.0"

fetch "https://github.com/wmww/gtk4-layer-shell/archive/refs/tags/v$GLS_VERSION.tar.gz" "$WORK/gls.tar.gz" "$GLS_SHA256"
meson setup "$WORK/gls-build" "$WORK/gtk4-layer-shell-$GLS_VERSION" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Dintrospection=false -Dvapi=false -Ddocs=false -Dexamples=false -Dtests=false -Dsmoke-tests=false
ninja -C "$WORK/gls-build" install
cp -f "$WORK/SDL3-$SDL_VERSION/LICENSE.txt" "$PREFIX/LICENSE.SDL3.txt"
cp -f "$WORK/gtk4-layer-shell-$GLS_VERSION/LICENSE" "$PREFIX/LICENSE.gtk4-layer-shell.txt"
echo "SDL3 $SDL_VERSION and gtk4-layer-shell $GLS_VERSION installed to $PREFIX"
