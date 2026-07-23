#!/usr/bin/env bash
#
# Fetch Apogee's freely distributable Episode 1 and put its two group files
# where the build expects them.
#
# The checksums are pinned in the workflow. A release should never carry game
# data that nobody has checked, and the archive this comes from is a mirror
# rather than a publisher.
set -euo pipefail

target="${1:?usage: fetch-shareware.sh <directory>}"
mkdir -p "$target"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "fetching the shareware episode"
curl -sSL --retry 3 --retry-delay 5 -o "$work/shareware.zip" "$SHAREWARE_URL"
unzip -q -o "$work/shareware.zip" -d "$work/extracted"

# The archive's own layout is not guaranteed, so the files are found by name.
find "$work/extracted" -iname 'COSMO1.STN' -exec cp {} "$target/COSMO1.STN" \;
find "$work/extracted" -iname 'COSMO1.VOL' -exec cp {} "$target/COSMO1.VOL" \;

verify() {
    local file="$1" expected="$2" actual
    if command -v sha256sum >/dev/null; then
        actual="$(sha256sum "$file" | cut -d' ' -f1)"
    else
        actual="$(shasum -a 256 "$file" | cut -d' ' -f1)"
    fi

    if [ "$actual" != "$expected" ]; then
        echo "$file does not match what was expected" >&2
        echo "  expected $expected" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
    echo "  $(basename "$file") verified"
}

verify "$target/COSMO1.STN" "$STN_SHA256"
verify "$target/COSMO1.VOL" "$VOL_SHA256"
