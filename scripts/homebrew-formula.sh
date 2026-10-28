#!/usr/bin/env bash
# Prints the Homebrew formula for a release, with the checksums of its archives.
#
#   bash scripts/homebrew-formula.sh <version> <dist dir> [owner/repo] > culprit.rb
set -euo pipefail

version=${1:?usage: homebrew-formula.sh <version> <dist dir> [owner/repo]}
dist=${2:?usage: homebrew-formula.sh <version> <dist dir> [owner/repo]}
repo=${3:-amirizalrahmat0799/culprit}
base="https://github.com/$repo/releases/download/v$version"

for target in macos-universal linux-x64 linux-arm64; do
  [[ -f "$dist/culprit-$version-$target.tar.gz" ]] || { echo "missing $dist/culprit-$version-$target.tar.gz" >&2; exit 1; }
done

sha() {
  local file="$dist/culprit-$version-$1.tar.gz"
  if command -v sha256sum >/dev/null; then sha256sum "$file" | cut -d' ' -f1
  else shasum -a 256 "$file" | cut -d' ' -f1
  fi
}

cat <<EOF
class Culprit < Formula
  desc "Terminal log viewer that groups stack traces and points at your code"
  homepage "https://github.com/$repo"
  version "$version"
  license "MIT"

  on_macos do
    url "$base/culprit-$version-macos-universal.tar.gz"
    sha256 "$(sha macos-universal)"
  end

  on_linux do
    on_intel do
      url "$base/culprit-$version-linux-x64.tar.gz"
      sha256 "$(sha linux-x64)"
    end
    on_arm do
      url "$base/culprit-$version-linux-arm64.tar.gz"
      sha256 "$(sha linux-arm64)"
    end
  end

  def install
    bin.install "culprit"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/culprit --version")
  end
end
EOF
