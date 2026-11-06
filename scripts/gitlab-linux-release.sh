#!/bin/sh
# Runs inside the GitLab CI release job (see .gitlab-ci.yml): the files go to the package registry, the
# release links to them. BETA is 2.0.N, from the version job.
set -eu

ver="$BETA-beta"
api="$CI_API_V4_URL/projects/$CI_PROJECT_ID"
links=
for path in release/*; do
    file=${path##*/}
    url="$api/packages/generic/screenplus/$ver/$file"
    curl -fsS -H "JOB-TOKEN: $CI_JOB_TOKEN" --upload-file "$path" "$url" > /dev/null
    links="$links${links:+,}{\"name\":\"$file\",\"url\":\"$url\",\"direct_asset_path\":\"/$file\",\"link_type\":\"package\"}"
done
curl -fsS -H "JOB-TOKEN: $CI_JOB_TOKEN" -H 'Content-Type: application/json' "$api/releases" --data \
    "{\"name\":\"Screen+ $ver\",\"tag_name\":\"v$ver\",\"ref\":\"$CI_COMMIT_SHA\",\"description\":\"Linux beta, built from $CI_COMMIT_SHORT_SHA.\",\"assets\":{\"links\":[$links]}}" > /dev/null
echo "Released v$ver"
