#!/usr/bin/env bash
# Builds Screen- from the pinned Godot tag plus our modules (no Godot source is kept in this repo).
# Usage: build.sh <windows|linuxbsd|web> [editor|template_release|template_debug] [out-dir]
# Meant for CI: the clone and objects go to $SCREEN_MINUS_WORK (default .work/), which CI deletes afterwards.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tag=$(tr -d '[:space:]' < "$here/godot.pin")
platform=${1:?platform: windows, linuxbsd or web}
target=${2:-template_release}
out=${3:-$here/out}
work=${SCREEN_MINUS_WORK:-$here/.work}

mkdir -p "$work" "$out"
if [ ! -d "$work/godot/.git" ]; then
	git clone --quiet --depth 1 --branch "$tag" https://github.com/godotengine/godot.git "$work/godot"
fi
python -m pip install --quiet scons

extra=()
[ "$target" = editor ] || extra+=(production=yes)
[ "$platform" = windows ] && extra+=(d3d12=yes)
python -m SCons -C "$work/godot" -j"$(nproc 2>/dev/null || echo 8)" \
	platform="$platform" target="$target" custom_modules="$here/modules" "${extra[@]}"

# Godot names its output godot.<platform>.<target>...; ours is screen-minus (editor) or screen-runtime.
name=screen-runtime
[ "$target" = editor ] && name=screen-minus
for f in "$work"/godot/bin/godot."$platform"."$target"*; do
	[ -e "$f" ] || continue
	cp "$f" "$out/$name${f##*"$target"}"
done
ls -l "$out"
