#!/bin/sh
# Verify the project: build every preset, run its tests, and run the engine's own suite.
#
# The presets are separate build trees, so they are verified **in parallel** - that is most of the wall
# clock on a machine with cores to spare - and each preset's tests run in parallel too (ctest -j). The job
# budget is shared: each preset gets a share of the cores rather than all of them.
#
#   tools/verify.sh                        every mine preset (mefemma has its own presets)
#   tools/verify.sh mine-release mine-debug  only these
#   VERIFY_JOBS=8 tools/verify.sh          say how many cores to use (default: all of them)
#
# Logs land in build/verify/; the exit status is 0 only when every target passed.
set -u

cd "$(dirname "$0")/.." || exit 2
tile2d=$(pwd)
repo=$(dirname "$tile2d")
log_dir=$tile2d/build/verify
mkdir -p "$log_dir"

cores() {
    nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4
}
jobs=${VERIFY_JOBS:-$(cores)}

presets=${*:-mine-release mine-debug mine-asan mine-tsan no-renderer}
count=0
for preset in $presets; do count=$((count + 1)); done
ore_present=0
[ -d "$repo/build/release" ] && ore_present=1
[ "$ore_present" = 1 ] && count=$((count + 1))
share=$((jobs / count))
[ "$share" -lt 1 ] && share=1

pids=""
labels=""

# Starts one target in the background: a build and then its tests, both logged. Started from this shell
# rather than from a subshell, so that wait below waits for something it is the parent of.
# Where a preset's build tree lives: the two games have their own roots, the framework-only preset
# its own. The names are CMakePresets.json's, not guessed from the tree.
build_dir() {
    case $1 in
        no-renderer) echo "$tile2d/build/no-renderer" ;;
        mine-*) echo "$tile2d/build/mine/${1#mine-}" ;;
        mefemma-*) echo "$tile2d/build/mefemma/${1#mefemma-}" ;;
        *) echo "$tile2d/build/$1" ;;
    esac
}

start() {
    label=$1
    directory=$2
    preset=$3
    (
        # A preset that was never configured has no cache yet: configuring it here is what makes a
        # fresh checkout work with one command. The engine's own tree has its cache already and no
        # tile2d preset, so it is left alone (empty preset).
        if [ -n "$preset" ] && [ ! -f "$directory/CMakeCache.txt" ]; then
            cmake --preset "$preset" > "$log_dir/$label-configure.log" 2>&1 || exit 1
        fi
        cmake --build "$directory" -j "$share" || exit 1
        ctest --test-dir "$directory" -j "$share" || exit 1
    ) > "$log_dir/$label.log" 2>&1 &
    pids="$pids $!"
    labels="$labels $label"
}

started=$(date +%s)
for preset in $presets; do start "$preset" "$(build_dir "$preset")" "$preset"; done
[ "$ore_present" = 1 ] && start ore "$repo/build" ""

failed=0
set -- $labels
for pid in $pids; do
    label=$1
    shift
    if wait "$pid"; then
        summary=$(grep -o '[0-9]*% tests passed.*out of [0-9]*' "$log_dir/$label.log" | tail -1)
        [ -z "$summary" ] && summary='no tests ran'
        printf '  ok    %-12s %s\n' "$label" "$summary"
    else
        failed=$((failed + 1))
        printf '  FAIL  %-12s %s\n' "$label" "$(tail -2 "$log_dir/$label.log" | tr '\n' ' ')"
    fi
done

elapsed=$(( $(date +%s) - started ))
echo
if [ "$failed" = 0 ]; then
    echo "verify: all $count target(s) green in ${elapsed}s, $share core(s) each (logs in build/verify/)"
    exit 0
fi
echo "verify: $failed of $count target(s) failed in ${elapsed}s (logs in build/verify/)"
exit 1
