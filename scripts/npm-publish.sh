#!/usr/bin/env bash
# Stages the packages scripts/npm-packages.mjs made on npm, with trusted publishing (no token): the platform
# packages first, then culprit-cli. A staged version isn't public until a maintainer approves it with 2FA,
# on npmjs.com (Staged Packages) or with `npm stage approve <package>@<version>`.
#
# Approve the four platform packages before culprit-cli: culprit-cli points at them, and an install of
# culprit-cli finds no binary until its platform package is public.
#
# Versions already public are skipped, and so are versions already staged, so a failed run can be started again.
#
#   bash scripts/npm-publish.sh <output dir>
set -euo pipefail

out=$(cd "${1:?usage: npm-publish.sh <output dir>}" && pwd)
staged=()

stage() {
  local dir=$1 name version tag=latest log
  name=$(node -p 'require(process.argv[1]).name' "$dir/package.json")
  version=$(node -p 'require(process.argv[1]).version' "$dir/package.json")
  [[ $version == *-* ]] && tag=next   # 0.2.0-rc.1 doesn't become what `npm i culprit-cli` installs

  if npm view "$name@$version" version >/dev/null 2>&1; then
    echo "$name@$version is already public on npm"
    return
  fi
  echo "staging $name@$version ($tag)"
  log=$(mktemp)
  if (cd "$dir" && npm stage publish --access public --tag "$tag" ${GITHUB_ACTIONS:+--provenance}) 2>&1 | tee "$log"; then
    staged+=("$name@$version")
  elif grep -qiE "staged|already exists|cannot publish over" "$log"; then
    echo "$name@$version is already staged; approve it on npmjs.com"
    staged+=("$name@$version")
  else
    exit 1
  fi
}

for dir in "$out"/culprit-cli-*/; do
  stage "${dir%/}"
done
stage "$out/culprit-cli"

if [[ ${#staged[@]} -gt 0 ]]; then
  summary=${GITHUB_STEP_SUMMARY:-/dev/stdout}
  {
    echo "### Staged on npm: approve to make them public"
    echo "On npmjs.com > Staged Packages, or with these commands (each asks for your 2FA code)."
    echo "Approve culprit-cli last."
    echo '```'
    for p in "${staged[@]}"; do echo "npm stage approve $p"; done
    echo '```'
  } >> "$summary"
fi
