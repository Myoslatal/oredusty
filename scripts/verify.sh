#!/usr/bin/env bash
# Ore framework - one shot verification: configure, build, test and render reference screenshots.
#
#   ./scripts/verify.sh [preset]      # preset: debug (default) | release | asan | embedded
#
# Works without a GPU: when no hardware Vulkan device answers, a software implementation
# (lavapipe / SwiftShader) is selected automatically through VK_ICD_FILENAMES.
set -euo pipefail

preset="${1:-debug}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

echo "==> Ore verification (preset: $preset)"

if [[ -z "${VK_ICD_FILENAMES:-}" ]]; then
    if ! command -v vulkaninfo >/dev/null 2>&1 || ! timeout 20 vulkaninfo --summary >/dev/null 2>&1; then
        for candidate in \
            /usr/share/vulkan/icd.d/lvp_icd.*.json \
            /usr/lib/vulkan-swiftshader/vk_swiftshader_icd.json \
            /usr/lib/*/vk_swiftshader_icd.json \
            /usr/lib/chromium/vk_swiftshader_icd.json; do
            if [[ -f "$candidate" ]]; then
                export VK_ICD_FILENAMES="$candidate"
                echo "    no hardware Vulkan device detected, using software Vulkan: $candidate"
                break
            fi
        done
    fi
fi

echo "==> configure"
cmake --preset "$preset"

echo "==> build"
cmake --build "build/$preset"

echo "==> test"
ctest --test-dir "build/$preset" --output-on-failure

echo "==> render reference screenshots"
captures="build/$preset/captures"
mkdir -p "$captures"
for example in 01_triangle 02_cube 03_scene; do
    binary="build/$preset/examples/ore_example_$example"
    [[ -x "$binary" ]] || continue
    "$binary" --headless --size 1280x720 --frames 2 --screenshot "$captures/$example.png" >/dev/null
    echo "    $captures/$example.png"
done

echo "==> done: tests green, captures in $captures"
