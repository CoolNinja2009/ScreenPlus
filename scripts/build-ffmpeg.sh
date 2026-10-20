#!/usr/bin/env bash
# Builds the minimal, LGPL-2.1+ FFmpeg used by Screen+ (shared DLLs, no GPL/nonfree parts).
#
# Requirements: MSYS2 (UCRT64 environment: mingw-w64-ucrt-x86_64-gcc, make), NASM on PATH.
# Usage (from an MSYS2 UCRT64 shell, in the ScreenPlus directory):
#     scripts/build-ffmpeg.sh                # downloads ffmpeg-$FFMPEG_VERSION if needed
#     FFMPEG_VERSION=8.1.3 scripts/build-ffmpeg.sh
# Output: third_party/ffmpeg/{bin,include,lib}. MSVC import libraries (*.lib) are generated
# from the .def files by build.cmd (needs lib.exe from Visual Studio).
#
# Only the components needed for wallpapers are enabled: file input, common containers,
# H.264/HEVC/VP8/VP9/AV1(hw only)/MPEG-2/MPEG-4/VC-1/GIF video, common audio codecs,
# D3D11VA/DXVA2 hardware acceleration, libswscale, libswresample. Network is disabled.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-8.1.3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/build/_deps"
SRC="$DEPS/ffmpeg-$FFMPEG_VERSION"
PREFIX="$ROOT/third_party/ffmpeg"
JOBS="$(nproc 2>/dev/null || echo 4)"

mkdir -p "$DEPS"
if [ ! -f "$SRC/configure" ]; then
    tarball="$DEPS/ffmpeg-$FFMPEG_VERSION.tar.xz"
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    tar -xf "$tarball" -C "$DEPS"
fi

cd "$SRC"
./configure \
    --prefix="$PREFIX" \
    --target-os=mingw32 --arch=x86_64 \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-everything \
    --disable-avdevice --disable-avfilter --disable-network \
    --enable-w32threads --enable-d3d11va --enable-dxva2 \
    --enable-swscale --enable-swresample \
    --enable-protocol=file \
    --enable-demuxer=mov,matroska,avi,mpegts,mpegps,asf,flv,ivf,gif \
    --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,vc1,gif,aac,mpegaudio,opus,vorbis,flac,ac3 \
    --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg4,mpeg2video,mpeg1video,vc1,wmv3,gif,aac,mp3float,opus,vorbis,flac,ac3,eac3,alac,wmav2,pcm_s16le,pcm_s24le,pcm_f32le \
    --enable-hwaccel=h264_d3d11va,h264_d3d11va2,h264_dxva2,hevc_d3d11va,hevc_d3d11va2,hevc_dxva2,vp9_d3d11va,vp9_d3d11va2,vp9_dxva2,av1_d3d11va,av1_d3d11va2,av1_dxva2,mpeg2_d3d11va,mpeg2_d3d11va2,mpeg2_dxva2,vc1_d3d11va,vc1_d3d11va2,vc1_dxva2,wmv3_d3d11va,wmv3_d3d11va2,wmv3_dxva2 \
    --enable-bsf=vp9_superframe_split \
    --extra-ldflags="-static" \
    --x86asmexe=nasm

make -j"$JOBS"
# The DLLs must depend only on Windows system libraries (no libgcc/libwinpthread).
if command -v objdump >/dev/null; then ! objdump -p libavutil/avutil-*.dll | grep -qi "winpthread\|libgcc" || { echo "unexpected MinGW runtime dependency"; exit 1; }; fi
make install
cp -f "$SRC/COPYING.LGPLv2.1" "$PREFIX/COPYING.LGPLv2.1"
{ echo "FFmpeg $FFMPEG_VERSION (https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz)";
  grep -E '^#define FFMPEG_(CONFIGURATION|LICENSE)' config.h; } > "$PREFIX/BUILDINFO.txt"
echo "FFmpeg $FFMPEG_VERSION installed to $PREFIX"
