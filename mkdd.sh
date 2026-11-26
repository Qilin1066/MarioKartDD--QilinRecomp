#!/bin/sh
# Builds Mario Kart: Double Dash!! from your own disc and starts it: Linux (the Raspberry
# Pi 4 included) and macOS. The Windows counterparts are "Play Double Dash.bat"
# and "Update Double Dash.bat".
#
#   ./mkdd.sh                    start the game; build it first if it isn't built
#   ./mkdd.sh update             get the latest code (git pull), rebuild what changed, start
#   ./mkdd.sh build              build (or rebuild what changed) without starting
#   ./mkdd.sh --disc <path>      use this disc (remembered for next time)
#
# The disc is your own copy of the game (USA, GM4E01): an image (.iso, .gcm
# or .ciso) as it is, or a folder extracted from one (the one with sys/ and
# files/). The build is optimised for this machine's own
# CPU (QILIN_NATIVE) and runs as many compiles at once as the memory allows
# (about 1.5 GB each: a 4 GB Pi 4 runs two), so the first build takes 10 to
# 20 minutes on a desktop and about an hour on a Pi 4.
set -eu
root=$(cd "$(dirname "$0")" && pwd)
build="$root/build-game"
disc_file="$build/disc-folder.txt"
mode=play
disc=""
while [ $# -gt 0 ]; do
    case "$1" in
        update) mode=update ;;
        build) mode=build ;;
        play) mode=play ;;
        --disc) [ $# -ge 2 ] || { echo "--disc needs the disc image or the extracted disc's folder" >&2; exit 2; }; disc=$2; shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (see ./mkdd.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
stop() { printf '\nStopped: %s\n' "$1" >&2; exit 1; }

# The disc: given, or the one used last.
if [ -z "$disc" ] && [ -f "$disc_file" ]; then disc=$(cat "$disc_file"); fi
[ -n "$disc" ] || stop "which disc? Run ./mkdd.sh --disc <your disc image (.iso, .gcm, .ciso), or an extracted disc's folder>"
if [ -d "$disc" ]; then
    disc=$(cd "$disc" && pwd)
    [ -d "$disc/files" ] || stop "$disc has no files folder: extract the whole disc, or use the image itself"
    [ -f "$disc/sys/fst.bin" ] || stop "$disc has no sys/fst.bin: extract the whole disc, or use the image itself"
    [ -f "$disc/sys/main.dol" ] || stop "$disc has no sys/main.dol: extract the whole disc, or use the image itself"
elif [ -f "$disc" ]; then
    disc=$(cd "$(dirname "$disc")" && pwd)/$(basename "$disc")
else
    stop "the disc isn't there: $disc"
fi

exe="$build/mkdd"
if [ "$mode" = play ] && [ -x "$exe" ] && [ -f "$disc_file" ] && [ "$(cat "$disc_file")" = "$disc" ]; then
    exec "$exe" "$disc"
fi

if [ "$mode" = update ]; then
    command -v git >/dev/null || stop "git isn't installed"
    git -C "$root" pull --ff-only || stop "git pull failed (local changes?)"
fi

# Tools: CMake 3.25+, and GCC 14+ or Clang 19+.
command -v cmake >/dev/null || stop "CMake isn't installed (Debian, Ubuntu, Raspberry Pi OS: sudo apt install cmake)"
if [ -z "${CXX:-}" ]; then
    for c in g++-14 g++-15 clang++-19 clang++-20 clang++ g++; do
        command -v "$c" >/dev/null && { CXX=$c; break; }
    done
fi
[ -n "${CXX:-}" ] || stop "no C++ compiler: sudo apt install g++-14 (GCC 14 or newer, or Clang 19 or newer)"
if [ -z "${CC:-}" ]; then
case "$CXX" in
    *clang++*) CC=$(echo "$CXX" | sed 's/clang++/clang/') ;;
    *) CC=$(echo "$CXX" | sed 's/g++/gcc/') ;;
esac
fi
export CC CXX

# SDL3 is built in, from source: on Linux it needs the window system's and the
# sound system's headers.
if [ "$(uname -s)" = Linux ] && command -v dpkg >/dev/null; then
    missing=""
    for p in libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev; do
        dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"
    done
    [ -z "$missing" ] || stop "SDL3 needs these to build:$missing. Install them with: sudo apt install$missing"
fi

# Parallel compiles: one per core, at most one per 1.5 GB of memory.
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
if [ -r /proc/meminfo ]; then
    mem_kb=$(awk '/^MemAvailable:/ {available=$2} /^MemTotal:/ {total=$2} END {print available ? available : total}' /proc/meminfo)
    # Containers can see the host's RAM but only use their cgroup allowance.
    if [ -r /sys/fs/cgroup/memory.max ]; then
        limit=$(cat /sys/fs/cgroup/memory.max)
        used=$(cat /sys/fs/cgroup/memory.current 2>/dev/null || echo 0)
        case "$limit" in
            max|*[!0-9]*|"") ;;
            *) left=$(((limit - used) / 1024)); [ "$left" -ge 0 ] || left=0
               [ "$mem_kb" -le "$left" ] || mem_kb=$left ;;
        esac
    fi
    by_mem=$((mem_kb / 1500000))
    [ "$by_mem" -ge 1 ] || by_mem=1
    [ "$jobs" -le "$by_mem" ] || jobs=$by_mem
fi

mkdir -p "$build"
echo "Building Mario Kart: Double Dash!! with $CXX, $jobs at a time (the first build lifts the whole game to C++)."
# The disc is read where it lies (docs/disc.md): qilin-disc, built first, says what it is and
# copies its main.dol out for the build. The copy is replaced only when it differs, so the
# game's code is lifted again only for a different disc.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DQILIN_NATIVE=ON >/dev/null ||
    stop "CMake couldn't configure the build (its messages are above)"
cmake --build "$build" --target qilin_disc_tool --parallel "$jobs" >/dev/null || stop "the disc tool didn't build (run cmake --build $build --target qilin_disc_tool to see why)"
info=$("$build/qilin-disc" info "$disc" --json) || stop "that isn't a disc this can read (the message above says why)"
# (One "name": value a line; a value with or without quotes.)
field() { printf '%s\n' "$info" | tr -d '\r' | sed -n "s/^ *\"$1\": *\"\{0,1\}\([^\",]*\)\"\{0,1\},\{0,1\} *\$/\1/p"; }
[ "$(field id)" = GM4E01 ] && [ "$(field revision)" = 0 ] ||
    stop "that disc is $(field id), revision $(field revision): this port needs Mario Kart: Double Dash!! for the USA, GM4E01, revision 0"
[ "$(field dol_sha256)" = e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae ] ||
    stop "that disc's main.dol isn't the supported build's (SHA-256 $(field dol_sha256)): a modified or damaged copy?"
"$build/qilin-disc" dol "$disc" "$build/main.dol.new" >/dev/null || stop "the disc's main.dol couldn't be copied out"
if [ -f "$build/main.dol" ] && cmp -s "$build/main.dol" "$build/main.dol.new"; then rm -f "$build/main.dol.new"
else mv -f "$build/main.dol.new" "$build/main.dol"; fi
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DQILIN_NATIVE=ON -DQILIN_GAME_DOL="$build/main.dol" >/dev/null ||
    stop "CMake couldn't configure the build (its messages are above)"
cmake --build "$build" --target mkdd --parallel "$jobs" || stop "the build failed (its messages are above)"
# Remember only a successfully built disc. A failed change of disc must not
# make the fast path launch the old executable against the new disc next time.
printf '%s' "$disc" > "$disc_file.tmp"
mv -f "$disc_file.tmp" "$disc_file"
[ "$mode" = build ] && { echo "Built: $exe"; exit 0; }
exec "$exe" "$disc"
