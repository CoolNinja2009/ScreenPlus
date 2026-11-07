#!/bin/sh
# Prints the dotenv for every job of the GitLab pipeline (see .gitlab-ci.yml):
#   VERSION  the tag's in a tag pipeline, else the next patch after the newest vX.Y.Z tag, or the
#            CMakeLists.txt version when that is higher
#   SKIP     1 when the tag is already released (publishing a release creates its tag, and that tag's
#            pipeline has nothing left to do)
set -eu

if [ -n "${CI_COMMIT_TAG:-}" ]; then
    if curl -fsS -o /dev/null -H "JOB-TOKEN: $CI_JOB_TOKEN" \
        "$CI_API_V4_URL/projects/$CI_PROJECT_ID/releases/$CI_COMMIT_TAG" 2>/dev/null; then
        echo "$CI_COMMIT_TAG is already released" >&2
        printf 'VERSION=%s\nSKIP=1\n' "${CI_COMMIT_TAG#v}"
        exit 0
    fi
    printf 'VERSION=%s\nSKIP=0\n' "${CI_COMMIT_TAG#v}"
    exit 0
fi

base=$(sed -nE 's/^set\(SCREENPLUS_VERSION "([0-9]+\.[0-9]+\.[0-9]+)".*/\1/p' CMakeLists.txt)
[ -n "$base" ] || { echo "No version in CMakeLists.txt" >&2; exit 1; }
latest=$(git tag -l 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' | sort -V | tail -n 1)
if [ -z "$latest" ]; then
    ver=$base
elif [ "$base" != "$latest" ] && [ "$(printf '%s\n%s\n' "$base" "$latest" | sort -V | tail -n 1)" = "$base" ]; then
    ver=$base
else
    ver=$(echo "$latest" | awk -F. '{ printf "%d.%d.%d", $1, $2, $3 + 1 }')
fi
printf 'VERSION=%s\nSKIP=0\n' "$ver"
