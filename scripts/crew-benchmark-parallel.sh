#!/usr/bin/env bash
# Where the crew is SUPPOSED to win: parallelizable work.
#
# The simple benchmark (crew-benchmark.sh) showed the crew adds no accuracy on a
# single trivial task and costs ~7x the time. That's the crew's worst case. THIS
# one is its best case: one task that decomposes into N independent subtasks —
# "write a test file for each of these 5 modules" — where the Director can fan out
# N coders at once while a single agent grinds through them sequentially in one
# context. If multi-agent orchestration ever earns its cost, it is here.
#
# It measures two things per mode: COMPLETENESS (how many of the N subtasks got a
# working file) and WALL-CLOCK. Same task, same model, only the architecture
# differs — so any difference is the architecture.
#
#   scripts/crew-benchmark-parallel.sh
#   OLLAMA_MODEL=deepseek-v4-pro:cloud scripts/crew-benchmark-parallel.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ODV_CLI:-$ROOT/build/cli/ollamadev}"
[ -x "$CLI" ] || { echo "build the CLI first: cmake --build build" >&2; exit 1; }
if ! "$CLI" models >/dev/null 2>&1; then
    echo "⚠ no Ollama/model reachable — start Ollama and pull a model, then rerun."
    exit 0
fi

N=5
HAVE_PYTEST=0
python3 -m pytest --version >/dev/null 2>&1 && HAVE_PYTEST=1

# The N independent modules the agent must write a test for. Trivial and unrelated
# on purpose: the work is embarrassingly parallel, so any crew speedup is real.
seed_repo() {  # <dir>
    local d="$1"
    printf 'def add(a, b):\n    return a + b\n'        > "$d/mod1.py"
    printf 'def sub(a, b):\n    return a - b\n'        > "$d/mod2.py"
    printf 'def mul(a, b):\n    return a * b\n'        > "$d/mod3.py"
    printf 'def is_even(n):\n    return n %% 2 == 0\n' > "$d/mod4.py"
    printf 'def reverse(s):\n    return s[::-1]\n'     > "$d/mod5.py"
}

TASK="This directory contains 5 python modules: mod1.py, mod2.py, mod3.py, mod4.py, mod5.py. \
In the current directory, create a pytest test file for EACH module — test_mod1.py through \
test_mod5.py — where each imports its module and asserts the function behaves correctly. \
Write all five files to disk."

# How many of the N test files exist and actually pass. With pytest we check they
# pass; without it we count files that at least import and run without error.
score_repo() {  # <dir> -> "<passing>"
    local d="$1" pass=0 i f
    for i in $(seq 1 "$N"); do
        f="$d/test_mod${i}.py"
        [ -f "$f" ] || continue
        if [ "$HAVE_PYTEST" = 1 ]; then
            ( cd "$d" && python3 -m pytest -q "test_mod${i}.py" ) >/dev/null 2>&1 && pass=$((pass+1))
        else
            ( cd "$d" && python3 "test_mod${i}.py" ) >/dev/null 2>&1 && pass=$((pass+1))
        fi
    done
    echo "$pass"
}

run_one() {  # <mode> -> "<passing>/<N> <elapsed>s"
    local mode="$1"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git commit -q --allow-empty -m init )
    seed_repo "$tmp"
    local home="$tmp/home"; mkdir -p "$home"   # isolate: no active workspace hijack
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

echo "parallelizable benchmark — one task, $N independent subtasks"
[ "$HAVE_PYTEST" = 1 ] || echo "(pytest not installed — scoring by 'test file runs', not full assertions)"
echo
printf "%-8s | %-14s | %-14s\n" "mode" "completeness" "wall-clock"
printf -- "---------+----------------+---------------\n"
s="$(run_one single)"
c="$(run_one crew)"
printf "%-8s | %-14s | %-14s\n" "single" "${s%% *}" "${s##* }"
printf "%-8s | %-14s | %-14s\n" "crew"   "${c%% *}" "${c##* }"
echo
echo "single: $s   ·   crew: $c"
echo "(completeness = subtasks with a working test file; the crew's case is finishing MORE of them, or the same in less wall-clock via parallel coders)"
