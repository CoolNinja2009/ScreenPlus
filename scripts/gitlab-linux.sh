#!/usr/bin/env bash
# Runs inside the GitLab CI build jobs (see .gitlab-ci.yml). `tar` packs the folder for install.sh;
# `deb <distro>` and `rpm` make a system package, install it and run it. VERSION comes from the version job.
set -euo pipefail

kind=$1
distro=${2:-}
ver=$VERSION
echo "Building Screen+ $ver ($kind)"

if [ "$kind" = rpm ]; then
    # One .rpm for every Fedora: the depth module carries its own FFmpeg (the system's sonames differ per release)
    bash scripts/build-ffmpeg-linux.sh "$PWD/ffmpeg-own"
    export PKG_CONFIG_PATH=$PWD/ffmpeg-own/lib/pkgconfig
fi
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DSCREENPLUS_VERSION="$VERSION"
cmake --build build-linux
build-linux/src/linux/screenplus version
build-linux/src/linux/screenplus help > /dev/null
mkdir -p release

pack_tar() {
    local name="ScreenPlus-$ver-linux-x86_64"
    mkdir -p "dist/$name/fonts"
    cp build-linux/src/linux/screenplus build-linux/src/linux/screenplus-engine build-linux/src/linux/screenplus-ui.so build-linux/src/linux/screenplus-depth.so "dist/$name/"
    cp resources/depth.html resources/splat.html src/linux/install.sh src/linux/screenplus.png LICENSE THIRD_PARTY_NOTICES.md "dist/$name/"
    cp resources/fonts/*.ttf "dist/$name/fonts/"
    cp -r src/linux/kde samples "dist/$name/"
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
}

pack_deb() {
    stage
    local file="ScreenPlus-$ver-$distro-amd64.deb" libs
    local app="$pkg/usr/lib/screenplus"
    mkdir -p debian "$pkg/DEBIAN"
    touch debian/control          # dpkg-shlibdeps wants to be in a source package
    libs=$(dpkg-shlibdeps -O --warnings=0 -e"$app/screenplus" -e"$app/screenplus-engine" -e"$app/screenplus-ui.so" -e"$app/screenplus-depth.so")
    libs=${libs#shlibs:Depends=}
    cat > "$pkg/DEBIAN/control" <<CONTROL
Package: screenplus
Version: $VERSION
Architecture: amd64
Maintainer: Screen+ <noreply@screenplus.invalid>
Depends: $libs, ffmpeg, curl
Recommends: libarchive-tools, yt-dlp, qml6-module-qtmultimedia, qml6-module-qtwebengine
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
    # scripts/build-ffmpeg-linux.sh's FFmpeg beside the depth module (its RUNPATH is $ORIGIN/ffmpeg), each finding
    # the others next to itself; the package then requires no particular FFmpeg
    local ff=$pkg/usr/lib/screenplus/ffmpeg lib
    mkdir -p "$ff"
    cp -a ffmpeg-own/lib/*.so.* ffmpeg-own/COPYING.LGPLv2.1 "$ff/"
    for lib in "$ff"/*.so.*; do
        [ -L "$lib" ] || patchelf --set-rpath '$ORIGIN' "$lib"
    done
    rpmbuild -bb --define "_rpmdir $PWD/rpms" screenplus.spec
    cp rpms/x86_64/*.rpm "release/ScreenPlus-$ver-fedora-x86_64.rpm"
    dnf install -y "./release/ScreenPlus-$ver-fedora-x86_64.rpm"
}

case $kind in
    tar) pack_tar ;;
    deb) pack_deb ;;
    rpm) pack_rpm ;;
    *) echo "Unknown kind: $kind"; exit 1 ;;
esac

if [ "$kind" != tar ]; then
    /usr/bin/screenplus version | grep -F "$ver"
    /usr/bin/screenplus help > /dev/null
fi
ls -la release
