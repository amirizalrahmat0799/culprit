#!/usr/bin/env bash
# Publishes the packages scripts/npm-packages.mjs made: the platform packages first, then culprit-cli,
# so culprit-cli never points at a binary package that isn't there yet. Versions already on npm are
# skipped, so a failed run can be started again.
#
#   bash scripts/npm-publish.sh <output dir>
set -euo pipefail

out=$(cd "${1:?usage: npm-publish.sh <output dir>}" && pwd)

publish() {
  local dir=$1 name version tag=latest
  name=$(node -p 'require(process.argv[1]).name' "$dir/package.json")
  version=$(node -p 'require(process.argv[1]).version' "$dir/package.json")
  [[ $version == *-* ]] && tag=next   # 0.2.0-rc.1 doesn't become what `npm i culprit-cli` installs

  if npm view "$name@$version" version >/dev/null 2>&1; then
    echo "$name@$version is already on npm"
    return
  fi
  echo "publishing $name@$version ($tag)"
  (cd "$dir" && npm publish --access public --tag "$tag" ${GITHUB_ACTIONS:+--provenance})
}

for dir in "$out"/culprit-cli-*/; do
  publish "${dir%/}"
done
publish "$out/culprit-cli"
