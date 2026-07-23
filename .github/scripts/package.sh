#!/usr/bin/env bash
#
# Assemble a release archive for one platform.
#
#   package.sh <macos|linux|windows> <build directory> <version>
#
# Each archive holds what that platform needs to play and nothing else: the
# programs, Episode 1's data, and the two documents anyone is entitled to
# alongside somebody else's copyrighted game. No DOS executable, no headers, no
# build output beyond the binaries themselves.
set -euo pipefail

platform="${1:?platform}"
build="${2:?build directory}"
version="${3:?version}"

repo="$(cd "$(dirname "$0")/../.." && pwd)"
dist="$repo/dist"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$dist"

name="cosmo-${version}-${platform}"
root="$staging/$name"
mkdir -p "$root"

# Copy the two documents every archive carries.
docs() {
    cp "$repo/PLAYING.txt" "$1/"
    cp "$repo/LICENSE" "$1/LICENSE.txt"
    cp "$repo/ATTRIBUTION.md" "$1/"
}

case "$platform" in
macos)
    # Everything is already inside the bundle; the data goes in beside the
    # icon, which is where paths.c looks before falling back to the user's own
    # directory.
    cp -R "$build/Cosmo.app" "$root/"
    cp "$repo/gamedata/COSMO1.STN" "$repo/gamedata/COSMO1.VOL" \
       "$root/Cosmo.app/Contents/Resources/"
    docs "$root"

    # Ad-hoc signing. Not a developer certificate -- it does not stop Gatekeeper
    # asking -- but it gives the bundle a stable identity, without which macOS
    # refuses to start a universal binary it has rewritten.
    codesign --force --deep --sign - "$root/Cosmo.app" 2>/dev/null || true

    # ditto rather than zip: it is what preserves the bundle's symlinks and the
    # signature along with them.
    (cd "$staging" && ditto -c -k --keepParent "$name" "$dist/$name.zip")
    ;;

linux)
    cp "$build/cosmo" "$build/cosmo1" "$build/cosmo2" "$build/cosmo3" "$root/"
    cp "$repo/gamedata/COSMO1.STN" "$repo/gamedata/COSMO1.VOL" "$root/"
    chmod +x "$root"/cosmo*
    docs "$root"
    (cd "$staging" && tar czf "$dist/$name.tar.gz" "$name")
    ;;

windows)
    cp "$build/cosmo.exe" "$build/cosmo1.exe" "$build/cosmo2.exe" \
       "$build/cosmo3.exe" "$root/"
    cp "$repo/gamedata/COSMO1.STN" "$repo/gamedata/COSMO1.VOL" "$root/"
    docs "$root"
    (cd "$staging" && zip -qr "$dist/$name.zip" "$name")
    ;;

*)
    echo "unknown platform: $platform" >&2
    exit 1
    ;;
esac

echo "packaged:"
ls -la "$dist"
