#!/usr/bin/env bash
# Does the multi-agent crew actually beat a single agent? This measures it.
#
# For each task below it spins up a throwaway git repo, runs the task TWICE — once
# with a single agent (`ollamadev "<task>"`), once with the crew (`ollamadev crew
# "<task>"` then apply) — and checks the task's own verification command. It prints
# a pass/fail + wall-clock table so the "crew is better" claim is a number, not a
# vibe.
#
#   scripts/crew-benchmark.sh                 # all tasks, default model
#   OLLAMA_MODEL=qwen2.5-coder:7b scripts/crew-benchmark.sh
#
# Needs Ollama running with a tool-capable model. With none reachable it says so
# and exits 0 (nothing to measure) rather than reporting false failures.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ODV_CLI:-$ROOT/build/cli/ollamadev}"
[ -x "$CLI" ] || { echo "build the CLI first: cmake --build build" >&2; exit 1; }

# Preflight: is a model actually reachable? If not, don't pretend to benchmark.
if ! "$CLI" models >/dev/null 2>&1; then
    echo "⚠ no Ollama/model reachable — start Ollama and pull a model, then rerun."
    exit 0
fi

# Each task: a name, the instruction, the file it should produce, and a command
# that must exit 0 for the task to count as PASSED. Add rows freely — they are the
# benchmark. Kept tiny and self-checking so a weak local model still has a chance.
# The instruction is written as an unambiguous WRITE-TO-DISK task — "create a file
# X in the current directory" — so a single agent uses its edit tool instead of
# just explaining the code in prose. Both arms get the identical instruction, so
# the comparison is fair: same task, same model, only the architecture differs.
TASKS=(
  "fizzbuzz|In the current directory, create a file named fizzbuzz.py containing a function fizzbuzz(n) that returns 'Fizz','Buzz','FizzBuzz' or the number as a string. Write the file to disk.|fizzbuzz.py|python3 -c 'import fizzbuzz as f; assert f.fizzbuzz(3)==\"Fizz\" and f.fizzbuzz(5)==\"Buzz\" and f.fizzbuzz(15)==\"FizzBuzz\" and f.fizzbuzz(2)==\"2\"'"
  "palindrome|In the current directory, create a file named pal.py containing is_pal(s) that returns True iff s reads the same forwards and backwards, ignoring case. Write the file to disk.|pal.py|python3 -c 'import pal; assert pal.is_pal(\"RaceCar\") and not pal.is_pal(\"hello\")'"
  "wordcount|In the current directory, create a file named wc.py containing count_words(text) returning a dict of lowercased word -> count. Write the file to disk.|wc.py|python3 -c 'import wc; d=wc.count_words(\"a A b\"); assert d[\"a\"]==2 and d[\"b\"]==1'"
)

run_one() {  # <mode> <instruction> <verify>
    local mode="$1" instr="$2" verify="$3"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git commit -q --allow-empty -m init )
    # Isolated HOME per run: with no ~/.ollamadev/workspaces.json there is no "active
    # workspace" for the CLI to re-root into, so it stays in $tmp (its cwd) instead
    # of hijacking to whatever project was last open. Without this the single agent
    # silently ran in the wrong repo and every check failed.
    local home="$tmp/home"; mkdir -p "$home"
    local start; start="$(date +%s)"
    if [ "$mode" = single ]; then
        ( cd "$tmp" && HOME="$home" "$CLI" --backend ollama "$instr" >/dev/null 2>&1 )
    else
        ( cd "$tmp" && HOME="$home" "$CLI" crew "$instr" --land auto >/dev/null 2>&1 )
    fi
    local elapsed=$(( $(date +%s) - start ))
    local ok=FAIL
    ( cd "$tmp" && bash -c "$verify" ) >/dev/null 2>&1 && ok=pass
    rm -rf "$tmp"
    echo "$ok ${elapsed}s"
}

printf "%-14s | %-12s | %-12s\n" "task" "single" "crew"
printf -- "---------------+--------------+-------------\n"
sp=0; cp=0; n=0
for row in "${TASKS[@]}"; do
    IFS='|' read -r name instr _file verify <<<"$row"
    n=$((n+1))
    s="$(run_one single "$instr" "$verify")"
    c="$(run_one crew   "$instr" "$verify")"
    [ "${s%% *}" = pass ] && sp=$((sp+1))
    [ "${c%% *}" = pass ] && cp=$((cp+1))
    printf "%-14s | %-12s | %-12s\n" "$name" "$s" "$c"
done
printf -- "---------------+--------------+-------------\n"
printf "%-14s | %-12s | %-12s\n" "PASS RATE" "$sp/$n" "$cp/$n"
echo
echo "single agent: $sp/$n   ·   crew: $cp/$n"
