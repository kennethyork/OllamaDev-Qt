#!/usr/bin/env bash
# Where the crew is SUPPOSED to win: parallelizable C++ work.
#
# crew-benchmark.sh showed the crew adds no accuracy on a single trivial task and
# costs ~7x the time — its worst case. THIS is its best case: one task that splits
# into N independent subtasks (write N unrelated C++ headers), so the Director can
# fan out N coders at once while a single agent writes them sequentially in one
# context. If multi-agent orchestration ever earns its cost, it is here.
#
# Oracle is the compiler: for each of the N headers, compile a fixed test main and
# run it. Measures COMPLETENESS (how many of N compile + pass) and WALL-CLOCK.
# Same task, same model, same oracle — only the architecture differs.
#
#   OLLAMA_MODEL=deepseek-v4-pro:cloud scripts/crew-benchmark-parallel.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ODV_CLI:-$ROOT/build/cli/ollamadev}"
CXX="${CXX:-g++}"
[ -x "$CLI" ] || { echo "build the CLI first: cmake --build build" >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "need a C++ compiler ($CXX)" >&2; exit 1; }
if ! "$CLI" models >/dev/null 2>&1; then
    echo "⚠ no Ollama/model reachable — start Ollama and pull a model, then rerun."
    exit 0
fi

# N independent modules: header | signature (for the instruction) | assert body.
MODULES=(
  "sum.hpp|int sum(int a, int b) returning a+b|assert(sum(2,3)==5);"
  "diff.hpp|int diff(int a, int b) returning a-b|assert(diff(5,2)==3);"
  "prod.hpp|int prod(int a, int b) returning a*b|assert(prod(3,4)==12);"
  "maxi.hpp|int maxi(int a, int b) returning the larger|assert(maxi(3,7)==7);"
  "mini.hpp|int mini(int a, int b) returning the smaller|assert(mini(3,7)==3);"
)
N="${#MODULES[@]}"

build_task() {
    local t="In the current directory, create these ${N} independent C++ header files, each defining exactly the stated function, each defined INLINE (header-only, no separate .cpp), and write every file to disk:"
    local m header sig _
    for m in "${MODULES[@]}"; do
        IFS='|' read -r header sig _ <<<"$m"
        t="$t"$'\n'"- $header defining $sig"
    done
    echo "$t"
}
TASK="$(build_task)"

score_repo() {  # <dir> -> "<passing>"
    local d="$1" pass=0 m header sig body
    for m in "${MODULES[@]}"; do
        IFS='|' read -r header sig body <<<"$m"
        [ -f "$d/$header" ] || continue
        {
            printf '#include "%s"\n#include <cassert>\n' "$header"
            printf 'int main(){ %s return 0; }\n' "$body"
        } > "$d/_t_${header%.hpp}.cpp"
        ( cd "$d" && "$CXX" -std=c++17 -I. "_t_${header%.hpp}.cpp" -o "_t_${header%.hpp}" ) >/dev/null 2>&1 || continue
        ( cd "$d" && "./_t_${header%.hpp}" ) >/dev/null 2>&1 && pass=$((pass+1))
    done
    echo "$pass"
}

run_one() {  # <mode> -> "<passing>/<N> <elapsed>s"
    local mode="$1"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git commit -q --allow-empty -m init )
    local home="$tmp/home"; mkdir -p "$home"   # isolate: no active-workspace hijack
    local start; start="$(date +%s)"
    if [ "$mode" = single ]; then
        ( cd "$tmp" && HOME="$home" "$CLI" --backend ollama "$TASK" >/dev/null 2>&1 )
    else
        ( cd "$tmp" && HOME="$home" "$CLI" crew "$TASK" --land auto >/dev/null 2>&1 )
    fi
    local elapsed=$(( $(date +%s) - start ))
    local pass; pass="$(score_repo "$tmp")"
    rm -rf "$tmp"
    echo "$pass/$N ${elapsed}s"
}

echo "parallelizable C++ benchmark — one task, $N independent header subtasks"
echo "oracle: g++ compile + run asserts per header"
echo
printf "%-8s | %-14s | %-14s\n" "mode" "completeness" "wall-clock"
printf -- "---------+----------------+---------------\n"
s="$(run_one single)"
c="$(run_one crew)"
printf "%-8s | %-14s | %-14s\n" "single" "${s%% *}" "${s##* }"
printf "%-8s | %-14s | %-14s\n" "crew"   "${c%% *}" "${c##* }"
echo
echo "single: $s   ·   crew: $c"
echo "(crew's case: finishing MORE of the $N, or the same in less wall-clock via parallel coders)"
