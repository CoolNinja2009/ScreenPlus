#!/usr/bin/env bash
# Runs inside the GitLab CI job (see .gitlab-ci.yml): picks the next 2.0.N-beta, builds, packages, releases.
set -euo pipefail

git config --global --add safe.directory "$CI_PROJECT_DIR"
last=$(git tag -l 'v2.0.*-beta' | sed -E 's/^v2\.0\.([0-9]+)-beta$/\1/' | sort -n | tail -1)
num="2.0.$((${last:--1} + 1))"
ver="$num-beta"
echo "Building Screen+ $ver"

cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCREENPLUS_VERSION="$num" -DSCREENPLUS_SUFFIX=-beta
cmake --build build-linux
build-linux/src/linux/screenplus version
build-linux/src/linux/screenplus help > /dev/null

name="ScreenPlus-$ver-linux-x86_64"
mkdir -p "dist/$name/fonts" release
cp build-linux/src/linux/screenplus build-linux/src/linux/screenplus-engine build-linux/src/linux/screenplus-ui.so build-linux/src/linux/screenplus-depth.so "dist/$name/"
cp resources/depth.html resources/splat.html src/linux/install.sh src/linux/screenplus.png LICENSE THIRD_PARTY_NOTICES.md "dist/$name/"
cp resources/fonts/*.ttf "dist/$name/fonts/"
cp -r src/linux/kde samples "dist/$name/"
chmod +x "dist/$name/install.sh"
tar -C dist -czf "release/$name.tar.gz" "$name"
(cd release && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")

# A hand-started pipeline on another branch is built but not released.
[ "${CI_COMMIT_BRANCH:-}" = cross-platform ] || exit 0

# The files go to the package registry, the release links to them.
api="$CI_API_V4_URL/projects/$CI_PROJECT_ID"
links=
for file in "$name.tar.gz" "$name.tar.gz.sha256"; do
    url="$api/packages/generic/screenplus/$ver/$file"
    curl -fsS -H "JOB-TOKEN: $CI_JOB_TOKEN" --upload-file "release/$file" "$url" > /dev/null
    links="$links${links:+,}{\"name\":\"$file\",\"url\":\"$url\",\"direct_asset_path\":\"/$file\",\"link_type\":\"package\"}"
done
curl -fsS -H "JOB-TOKEN: $CI_JOB_TOKEN" -H 'Content-Type: application/json' "$api/releases" --data \
    "{\"name\":\"Screen+ $ver\",\"tag_name\":\"v$ver\",\"ref\":\"$CI_COMMIT_SHA\",\"description\":\"Linux beta, built from $CI_COMMIT_SHORT_SHA.\",\"assets\":{\"links\":[$links]}}" > /dev/null
echo "Released v$ver"
