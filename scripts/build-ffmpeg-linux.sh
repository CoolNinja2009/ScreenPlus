#!/usr/bin/env bash
# Builds a small, LGPL-2.1+ FFmpeg as shared libraries for screenplus-depth.so on Linux, so a package doesn't
# depend on the distribution's FFmpeg, whose sonames change from one release to the next (a .rpm built on
# Fedora 45 wouldn't install on 44). Only what depth_infer.c and wic_linux.c read and write: pictures (JPEG,
# PNG, BMP, WebP, GIF, AVIF) and H.264/HEVC/VP8/VP9/AV1 videos in, JPEG and PNG out. AV1 comes from the system's
# dav1d, zlib from the system's, both stable sonames.
#
# Needs gcc, make, nasm, pkg-config, dav1d and zlib headers. Usage: scripts/build-ffmpeg-linux.sh <prefix>
# then configure CMake with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig and ship <prefix>/lib/*.so.* in ffmpeg/
# beside screenplus-depth.so (its RUNPATH). Shared, like the Windows DLLs, so the LGPL libraries stay replaceable.
set -euo pipefail

FFMPEG_VERSION=8.1.3
FFMPEG_SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
PREFIX=$(realpath -m "${1:?prefix}")
WORK=$PREFIX.src
JOBS=$(nproc 2>/dev/null || echo 4)

mkdir -p "$WORK"
tarball=$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz
[ -f "$tarball" ] || curl -L --fail -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
echo "$FFMPEG_SHA256  $tarball" | sha256sum -c -
rm -rf "$WORK/ffmpeg-$FFMPEG_VERSION"
tar -xf "$tarball" -C "$WORK"

cd "$WORK/ffmpeg-$FFMPEG_VERSION"
./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-everything \
    --disable-avdevice --disable-avfilter --disable-network \
    --enable-pthreads --enable-zlib --enable-libdav1d \
    --enable-swscale --enable-swresample \
    --enable-protocol=file \
    --enable-demuxer=mov,matroska,avi,mpegts,ivf,gif,image2,image_jpeg_pipe,image_png_pipe,image_bmp_pipe,image_webp_pipe,image_gif_pipe \
    --enable-parser=h264,hevc,vp8,vp9,av1,mjpeg,png,bmp,webp,gif \
    --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mjpeg,png,bmp,webp,gif \
    --enable-encoder=mjpeg,png \
    --enable-bsf=vp9_superframe_split
make -j"$JOBS"
make install
cp -f COPYING.LGPLv2.1 "$PREFIX/COPYING.LGPLv2.1"
echo "FFmpeg $FFMPEG_VERSION installed to $PREFIX"
