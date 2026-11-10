#!/usr/bin/env bash
# Runs inside the GitLab CI build jobs (see .gitlab-ci.yml). `deb <distro>` and `rpm` make a system package, install it
# and run it; `deb ubuntu` also packs the tarball (the folder for install.sh) from the same tree, so it carries the same
# libraries and runs on any glibc 2.39+ distro. `build` only builds. VERSION comes from the version job.
set -euo pipefail

kind=$1
distro=${2:-}
ver=$VERSION
echo "Building Screen+ $ver ($kind)"

# One .rpm for every Fedora and one Ubuntu .deb for 24.04 (KDE neon's base) and newer: the depth module carries its own
# FFmpeg (the system's sonames differ per release). Ubuntu 24.04 has no SDL3 or gtk4-layer-shell, so that .deb carries
# them too (beside the program, the RUNPATH of screenplus-ui.so and screenplus-engine).
# They come from the CI cache (.gitlab-ci.yml, keyed on their build scripts) and are compiled again only when the cache
# has none for this script, REBUILD_LIBS=1 is set, or Screen+ fails to build against them.
own=
if [ "$kind" = rpm ] || [ "$distro" = ubuntu ]; then
    own=1
    export PKG_CONFIG_PATH=$PWD/ffmpeg-own/lib/pkgconfig
fi
if [ "$distro" = ubuntu ]; then
    export PKG_CONFIG_PATH=$PWD/libs-own/lib/pkgconfig:$PKG_CONFIG_PATH
fi

own_lib() {         # prefix script [force]: the cached build when it matches the script, else build it
    local stamp
    stamp=$(sha256sum "$2" | cut -d' ' -f1)
    if [ -z "${3:-}" ] && [ "${REBUILD_LIBS:-}" != 1 ] && [ "$(cat "$1/.built" 2>/dev/null)" = "$stamp" ]; then
        echo "Using the cached $1"
        return
    fi
    echo "Building $1"
    rm -rf "$1" "$1.src"
    bash "$2" "$PWD/$1"
    echo "$stamp" > "$1/.built"
    rm -rf "$1.src"
}
own_libs() {
    if [ -n "$own" ]; then own_lib ffmpeg-own scripts/build-ffmpeg-linux.sh "${1:-}"; fi
    if [ "$distro" = ubuntu ]; then own_lib libs-own scripts/build-libs-linux.sh "${1:-}"; fi
}
build_app() {
    rm -rf build-linux
    cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DSCREENPLUS_VERSION="$VERSION" &&
        cmake --build build-linux &&
        build-linux/src/linux/screenplus version &&
        build-linux/src/linux/screenplus help > /dev/null
}
own_libs
if ! build_app; then
    [ -n "$own" ] || exit 1
    echo "Screen+ didn't build against the cached libraries: compiling them again"
    own_libs force
    build_app
fi
mkdir -p release

pack_tar() {        # after stage: the installed tree, whose RUNPATHs are $ORIGIN
    local name="ScreenPlus-$ver-linux-x86_64"
    mkdir -p dist
    cp -a "$pkg/usr/lib/screenplus" "dist/$name"
    cp src/linux/install.sh src/linux/screenplus.png LICENSE THIRD_PARTY_NOTICES.md "dist/$name/"
    if [ -d libs-own ]; then cp libs-own/LICENSE.*.txt "dist/$name/"; fi
    chmod +x "dist/$name/install.sh"
    tar -C dist -czf "release/$name.tar.gz" "$name"
    (cd release && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
}

# The tree both packages hold: the app in /usr/lib/screenplus, which finds its files next to itself.
# No update.sh there, so the package manager does the updating.
stage() {
    pkg=$PWD/pkg
    DESTDIR=$pkg cmake --install build-linux --strip
    mkdir -p "$pkg/usr/bin" "$pkg/usr/share/applications" "$pkg/usr/share/icons/hicolor/256x256/apps" "$pkg/usr/share/doc/screenplus"
    ln -s ../lib/screenplus/screenplus "$pkg/usr/bin/screenplus"
    cp src/linux/screenplus.png "$pkg/usr/share/icons/hicolor/256x256/apps/org.screenplus.ScreenPlus.png"
    cp LICENSE THIRD_PARTY_NOTICES.md "$pkg/usr/share/doc/screenplus/"
    cat > "$pkg/usr/share/applications/org.screenplus.ScreenPlus.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Screen+
Comment=Live wallpapers
Exec=/usr/bin/screenplus show
Icon=org.screenplus.ScreenPlus
Categories=Utility;
DESKTOP
    [ -z "$own" ] && return
    # scripts/build-ffmpeg-linux.sh's FFmpeg beside the depth module (its RUNPATH is $ORIGIN/ffmpeg), each finding
    # the others next to itself; the package then requires no particular FFmpeg
    local ff=$pkg/usr/lib/screenplus/ffmpeg lib
    mkdir -p "$ff"
    cp -a ffmpeg-own/lib/*.so.* ffmpeg-own/COPYING.LGPLv2.1 "$ff/"
    for lib in "$ff"/*.so.*; do
        [ -L "$lib" ] || patchelf --set-rpath '$ORIGIN' "$lib"
    done
    if [ -d libs-own ]; then
        cp -a libs-own/lib/lib*.so.* "$pkg/usr/lib/screenplus/"
        cp libs-own/LICENSE.*.txt "$pkg/usr/share/doc/screenplus/"
    fi
}

pack_deb() {
    stage
    local file="ScreenPlus-$ver-$distro-amd64.deb" libs
    local app="$pkg/usr/lib/screenplus"
    mkdir -p debian "$pkg/DEBIAN"
    touch debian/control          # dpkg-shlibdeps wants to be in a source package
    local opt=(-O --warnings=0) bin=("$app/screenplus" "$app/screenplus-engine" "$app/screenplus-ui.so" "$app/screenplus-depth.so")
    if [ -n "$own" ]; then
        # Libraries the package carries itself have no package to depend on; what they need is counted
        opt+=(--ignore-missing-info -l"$app" -l"$app/ffmpeg")
        bin+=($(find "$app" -name 'lib*.so.*' -type f))
    fi
    libs=$(dpkg-shlibdeps "${opt[@]}" "${bin[@]/#/-e}")
    libs=${libs#shlibs:Depends=}
    cat > "$pkg/DEBIAN/control" <<CONTROL
Package: screenplus
Version: $VERSION
Architecture: amd64
Maintainer: Screen+ <noreply@screenplus.invalid>
Depends: $libs, ffmpeg, curl
Recommends: libarchive-tools, yt-dlp, qt6-multimedia | qml6-module-qtmultimedia, qt6-webengine | qml6-module-qtwebengine
Section: x11
Priority: optional
Homepage: https://gitlab.com/screenplus-group/screenplus
Description: Live wallpapers
CONTROL
    dpkg-deb --root-owner-group --build "$pkg" "release/$file"
    apt-get install -y --no-install-recommends "./release/$file"
}

pack_rpm() {
    stage
    cat > screenplus.spec <<SPEC
Name: screenplus
Version: $VERSION
Release: 1
Summary: Live wallpapers
License: MIT
URL: https://gitlab.com/screenplus-group/screenplus
Requires: /usr/bin/ffmpeg
Requires: curl
Recommends: bsdtar
Recommends: yt-dlp
Recommends: qt6-qtmultimedia
Recommends: qt6-qtwebengine
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
%global __provides_exclude_from ^/usr/lib/screenplus/.*\$
%global __requires_exclude ^lib(avcodec|avformat|avutil|swscale|swresample)\\.so
%description
Live wallpapers.
%install
cp -a $pkg/. %{buildroot}/
%files
/usr/bin/screenplus
/usr/lib/screenplus
/usr/share/applications/org.screenplus.ScreenPlus.desktop
/usr/share/icons/hicolor/256x256/apps/org.screenplus.ScreenPlus.png
/usr/share/doc/screenplus
SPEC
    rpmbuild -bb --define "_rpmdir $PWD/rpms" screenplus.spec
    cp rpms/x86_64/*.rpm "release/ScreenPlus-$ver-fedora-x86_64.rpm"
    dnf install -y "./release/ScreenPlus-$ver-fedora-x86_64.rpm"
}

case $kind in
    build) ;;
    deb) pack_deb; if [ "$distro" = ubuntu ]; then pack_tar; fi ;;
    rpm) pack_rpm ;;
    *) echo "Unknown kind: $kind"; exit 1 ;;
esac

if [ "$kind" != build ]; then
    /usr/bin/screenplus version | grep -F "$ver"
    /usr/bin/screenplus help > /dev/null
    if ldd /usr/lib/screenplus/screenplus-ui.so /usr/lib/screenplus/screenplus-engine /usr/lib/screenplus/screenplus-depth.so | grep 'not found'; then exit 1; fi
fi
ls -la release
