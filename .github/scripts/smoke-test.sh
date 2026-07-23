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

# Waiting on that process would measure the wrong thing. Windows _execv ends the
# launcher and starts the episode as something else entirely, so the launcher
# exits within a second of a perfectly successful handover. What matters is
# whether the episode program is still there, asked for by name.
sleep 25

if pgrep -f "cosmo1" > /dev/null 2>&1; then
    episode_alive=yes
else
    episode_alive=no
fi

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

# "delivered=" counts timer interrupts the game's own loop consumed. A number
# above zero means the 1992 code is running, not merely that a process started.
delivered="$(grep -o 'delivered=[0-9]*' "$log" | tail -1 | cut -d= -f2 || true)"

if [ -z "$delivered" ] || [ "$delivered" -le 0 ]; then
    echo "the game never ran: no timer interrupts were delivered" >&2
    exit 1
fi

echo "the game ran and consumed $delivered timer interrupts"

if [ "$episode_alive" = yes ]; then
    echo "and it was still playing when the time was up"
    exit 0
fi

echo "the episode program is gone" >&2

if grep -q 'the game returned' "$log"; then
    echo "it finished on its own, which is not what it was asked to do" >&2
else
    echo "with nothing logged on the way out, so it died rather than quit" >&2
fi
exit 1
