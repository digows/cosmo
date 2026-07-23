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
macos)   program="$(find "$work" -type f -path '*/Cosmo.app/Contents/MacOS/Cosmo')" ;;
linux)   program="$(find "$work" -type f -name cosmo -perm -u+x | head -1)" ;;
windows) program="$(find "$work" -type f -name 'cosmo.exe' | head -1)" ;;
*)       echo "unknown platform: $platform" >&2; exit 1 ;;
esac

if [ -z "$program" ] || [ ! -x "$program" ]; then
    echo "no launcher found in $archive" >&2
    exit 1
fi

echo "running $program"

log="$work/run.log"

# Reaching the title screen is not the same as playing. This presses through
# the main menu into a new game, which is where the Windows build was reported
# to die and where nothing before this would have noticed.
script="$work/begin.txt"
cat > "$script" <<'EOF'
9000 tap space
10500 tap b
12000 tap 1
EOF

# Episode 1 by name, so the launcher hands straight over rather than waiting on
# a menu nobody is watching. On Windows the program has no streams of its own,
# so COSMO_LOG is where its output has to come from.
(
    cd "$(dirname "$program")"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy COSMO_DEBUG=1 \
    COSMO_SCRIPT="$script" COSMO_LOG="$log" \
        "$program" 1 > "$log.stdout" 2>&1
) &
pid=$!

# "delivered=" counts the timer interrupts the game's own loop has consumed, and
# the game prints it to an unbuffered stream as it runs. Whether that number is
# still climbing is the one sign of life that means the same thing on every
# platform. Asking the OS for the process does not: on Windows the launcher
# _execv's into the episode, which then runs as a native process that MSYS
# pgrep cannot see, so a perfectly healthy game looks gone.
last_delivered() {
    grep -o 'delivered=[0-9]*' "$log" 2>/dev/null | tail -1 | cut -d= -f2
}

# Give it time to press through the menu and load a level -- which is where the
# crash was -- then read the clock, wait, and read it again. Play keeps the
# number moving; a crash freezes it.
sleep 20
before="$(last_delivered)"
sleep 6
after="$(last_delivered)"

# Best-effort cleanup, both ways: taskkill reaches the native Windows process
# the launcher started, pkill reaches the episode everywhere else.
taskkill //F //IM cosmo1.exe > /dev/null 2>&1 || true
pkill -9 -f "cosmo1" 2>/dev/null || true
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

cat "$log.stdout" >> "$log" 2>/dev/null || true

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

before="${before:-0}"
after="${after:-0}"

if [ "$after" -le 0 ]; then
    echo "the game never ran: no timer interrupts were delivered" >&2
    exit 1
fi

echo "the game's clock read $before, then $after six seconds later"

if [ "$after" -gt "$before" ]; then
    echo "still advancing, so it was still playing"
    exit 0
fi

if grep -q 'the game returned' "$log"; then
    echo "it stopped on its own, which is not what it was asked to do" >&2
else
    echo "the clock stopped without a word, so it died rather than quit" >&2
fi
exit 1
