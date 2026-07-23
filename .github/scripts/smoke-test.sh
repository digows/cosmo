#!/usr/bin/env bash
#
# Start the packaged game and check that it actually runs.
#
#   smoke-test.sh <macos|linux> <archive>
#
# v0.2.0 shipped a macOS bundle whose launcher drew its menu and then could not
# start an episode: SDL_GetBasePath() returns Contents/Resources inside a
# bundle, and the episode programs are in Contents/MacOS. Everything looked
# right in a screenshot of the menu, which is exactly why a screenshot of the
# menu is not enough.
#
# So this unpacks what is about to be published, runs it with no window and no
# sound, and insists on evidence that the game itself is running: the timer
# interrupt being delivered is something only the game's own loop produces.
set -euo pipefail

platform="${1:?platform}"
archive="${2:?archive}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

case "$archive" in
*.tar.gz) tar xzf "$archive" -C "$work" ;;
*.zip)    unzip -q "$archive" -d "$work" ;;
*)        echo "unknown archive: $archive" >&2; exit 1 ;;
esac

case "$platform" in
macos) program="$(find "$work" -type f -path '*/Cosmo.app/Contents/MacOS/Cosmo')" ;;
linux) program="$(find "$work" -type f -name cosmo -perm -u+x | head -1)" ;;
*)     echo "unknown platform: $platform" >&2; exit 1 ;;
esac

if [ -z "$program" ] || [ ! -x "$program" ]; then
    echo "no launcher found in $archive" >&2
    exit 1
fi

echo "running $program"

log="$work/run.log"

# Episode 1 by name, so the launcher hands straight over rather than waiting on
# a menu nobody is watching.
(
    cd "$(dirname "$program")"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy COSMO_DEBUG=1 \
        "$program" 1 > "$log" 2>&1
) &
pid=$!

sleep 20
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "--- output"
cat "$log" || true
echo "---"

if grep -q 'cannot start' "$log"; then
    echo "the launcher could not start the episode" >&2
    exit 1
fi

if grep -q 'No episode data' "$log"; then
    echo "the game did not find its data" >&2
    exit 1
fi

# "delivered=" counts timer interrupts the game's own loop consumed. A number
# above zero means the 1992 code is running, not merely that a process started.
delivered="$(grep -o 'delivered=[0-9]*' "$log" | tail -1 | cut -d= -f2 || true)"

if [ -z "$delivered" ] || [ "$delivered" -le 0 ]; then
    echo "the game never ran: no timer interrupts were delivered" >&2
    exit 1
fi

echo "the game ran, and consumed $delivered timer interrupts"
