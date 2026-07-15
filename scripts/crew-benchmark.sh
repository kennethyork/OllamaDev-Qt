#!/usr/bin/env bash
# Does the multi-agent crew actually beat a single agent? This measures it — on
# C++, because that is what this project is, and because a COMPILER is the most
# reliable oracle there is: either the code compiles and the asserts pass, or it
# doesn't. No test-runner, no import-style, no interpreter quirks to get wrong.
#
# For each task it spins up a throwaway repo, runs the task TWICE — once with a
# single agent (`ollamadev "<task>"`), once with the crew — then compiles a fixed
# test harness against the agent's header and runs it. Same task, same model, same
# oracle for both arms, so any difference is the architecture.
#
#   scripts/crew-benchmark.sh
#   OLLAMA_MODEL=deepseek-v4-pro:cloud scripts/crew-benchmark.sh
#
# Needs Ollama with a tool-capable model, and g++. With no model reachable it says
# so and exits 0 rather than reporting false failures.
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

# Each task: name | instruction (an unambiguous WRITE-A-HEADER task, identical for
# both arms) | the header file it must produce | the body of a test main() that is
# compiled against that header and must exit 0. The instruction states the exact
# signature so the fixed harness lines up. Add rows freely — they are the benchmark.
TASKS=(
  "add|In the current directory, create a C++ header file add.hpp defining a function int add(int a, int b) that returns a+b. Define the function INLINE in the header (header-only, no separate .cpp). Write the file to disk.|add.hpp|assert(add(2,3)==5); assert(add(-2,2)==0);"
  "clamp|In the current directory, create a C++ header file clamp.hpp defining int clamp_int(int x, int lo, int hi) that returns x clamped into [lo,hi]. Define the function INLINE in the header (header-only, no separate .cpp). Write the file to disk.|clamp.hpp|assert(clamp_int(5,0,10)==5); assert(clamp_int(-3,0,10)==0); assert(clamp_int(99,0,10)==10);"
  "palindrome|In the current directory, create a C++ header file pal.hpp that includes <string> and defines bool is_palindrome(const std::string& s), true iff s reads the same forwards and backwards. Define the function INLINE in the header (header-only, no separate .cpp). Write the file to disk.|pal.hpp|assert(is_palindrome(std::string(\"racecar\"))); assert(!is_palindrome(std::string(\"hello\")));"
)

# Compile a test main that includes the agent's header and runs the asserts.
verify() {  # <dir> <header> <assert-body> -> 0 == pass
    local d="$1" header="$2" body="$3"
    [ -f "$d/$header" ] || return 1
    {
        printf '#include "%s"\n' "$header"
        printf '#include <cassert>\n#include <string>\n'
        printf 'int main(){ %s return 0; }\n' "$body"
    } > "$d/_bench_test.cpp"
    ( cd "$d" && "$CXX" -std=c++17 -I. _bench_test.cpp -o _bench_test ) >/dev/null 2>&1 || return 1
    ( cd "$d" && ./_bench_test ) >/dev/null 2>&1
}

run_one() {  # <mode> <instr> <header> <body> -> "<pass|FAIL> <elapsed>s"
    local mode="$1" instr="$2" header="$3" body="$4"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git commit -q --allow-empty -m init )
    # Isolated HOME: with no ~/.ollamadev/workspaces.json there is no "active
    # workspace" for the one-shot to re-root into, so it stays in $tmp instead of
    # hijacking to the last-open project.
    local home="$tmp/home"; mkdir -p "$home"
    local start; start="$(date +%s)"
    if [ "$mode" = single ]; then
        ( cd "$tmp" && HOME="$home" "$CLI" --backend ollama "$instr" >/dev/null 2>&1 )
    else
        ( cd "$tmp" && HOME="$home" "$CLI" crew "$instr" --land auto >/dev/null 2>&1 )
    fi
    local elapsed=$(( $(date +%s) - start ))
    local ok=FAIL
    verify "$tmp" "$header" "$body" && ok=pass
    rm -rf "$tmp"
    echo "$ok ${elapsed}s"
}

printf "%-12s | %-12s | %-12s\n" "task" "single" "crew"
printf -- "-------------+--------------+-------------\n"
sp=0; cp=0; n=0
for row in "${TASKS[@]}"; do
    IFS='|' read -r name instr header body <<<"$row"
    n=$((n+1))
    s="$(run_one single "$instr" "$header" "$body")"
    c="$(run_one crew   "$instr" "$header" "$body")"
    [ "${s%% *}" = pass ] && sp=$((sp+1))
    [ "${c%% *}" = pass ] && cp=$((cp+1))
    printf "%-12s | %-12s | %-12s\n" "$name" "$s" "$c"
done
printf -- "-------------+--------------+-------------\n"
printf "%-12s | %-12s | %-12s\n" "PASS RATE" "$sp/$n" "$cp/$n"
echo
echo "single agent: $sp/$n   ·   crew: $cp/$n   (oracle: g++ compile + run asserts)"
