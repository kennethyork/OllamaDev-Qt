#!/usr/bin/env bash
# Publishable crew-vs-single benchmark. Two things the quick benchmarks lacked:
#
#   1. TRIALS per cell (default 5) — LLMs are stochastic, so a single run is noise.
#      We report pass RATE (k/N) and mean±std wall-clock, not one lucky/unlucky roll.
#   2. A LARGE-subtask tier — the crew can only beat a single agent when each
#      subtask is expensive enough that running them in parallel outruns doing them
#      in sequence. Tier B's four subtasks are each real algorithms, not one-liners.
#
# Oracle is the C++ compiler: compile a fixed test main against the agent's header
# and run its asserts. Same task, model, and oracle for both arms — only the
# architecture differs. Reliable enough to put a number in a README.
#
#   TRIALS=5 OLLAMA_MODEL=deepseek-v4-pro:cloud scripts/crew-benchmark-publish.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ODV_CLI:-$ROOT/build/cli/ollamadev}"
CXX="${CXX:-g++}"
TRIALS="${TRIALS:-5}"
MODEL="${OLLAMA_MODEL:-$($CLI models 2>/dev/null | head -1 | tr -d ' ')}"
[ -x "$CLI" ] || { echo "build the CLI first: cmake --build build" >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "need a C++ compiler ($CXX)" >&2; exit 1; }
if ! "$CLI" models >/dev/null 2>&1; then
    echo "⚠ no Ollama/model reachable — start Ollama and pull a model, then rerun."
    exit 0
fi

# ---- tasks -----------------------------------------------------------------
# Tier A — small single-file tasks (crew's worst case: no room to parallelize).
#   header | instruction | assert body
SMALL=(
  "add.hpp|create a C++ header add.hpp defining int add(int a,int b) returning a+b|assert(add(2,3)==5); assert(add(-2,2)==0);"
  "clamp.hpp|create a C++ header clamp.hpp defining int clamp_int(int x,int lo,int hi) clamping x into [lo,hi]|assert(clamp_int(5,0,10)==5); assert(clamp_int(-3,0,10)==0); assert(clamp_int(99,0,10)==10);"
  "pal.hpp|create a C++ header pal.hpp that includes <string> and defines bool is_palindrome(const std::string& s)|assert(is_palindrome(std::string(\"racecar\"))); assert(!is_palindrome(std::string(\"hello\")));"
)

# Tier B — ONE task that splits into 4 EXPENSIVE, independent subtasks (crew's best
# case). Each is a real ~20-50 line algorithm, so a single agent must grind through
# them sequentially while the crew can fan out four coders at once.
#   header | signature-phrase | assert body
BIG=(
  "b64.hpp|std::string b64_encode(const std::string& in) — standard base64, include <string>|assert(b64_encode(std::string(\"Man\"))==std::string(\"TWFu\")); assert(b64_encode(std::string(\"\"))==std::string(\"\"));"
  "rle.hpp|std::string rle_encode(const std::string& s) — run-length encode as char+count, e.g. \\\"aaab\\\"->\\\"a3b1\\\", include <string>|assert(rle_encode(std::string(\"aaab\"))==std::string(\"a3b1\"));"
  "roman.hpp|std::string to_roman(int n) for 1..3999, include <string>|assert(to_roman(4)==std::string(\"IV\")); assert(to_roman(9)==std::string(\"IX\")); assert(to_roman(58)==std::string(\"LVIII\"));"
  "caesar.hpp|std::string caesar(const std::string& s,int shift) shifting ASCII letters, non-letters unchanged, include <string>|assert(caesar(std::string(\"abcXYZ\"),1)==std::string(\"bcdYZA\"));"
)

# ---- oracle ----------------------------------------------------------------
check_one() {  # <dir> <header> <assert-body> -> 0 == compiles & passes
    local d="$1" header="$2" body="$3"
    [ -f "$d/$header" ] || return 1
    { printf '#include "%s"\n#include <cassert>\n#include <string>\n' "$header"
      printf 'int main(){ %s return 0; }\n' "$body"; } > "$d/_t_${header%.hpp}.cpp"
    ( cd "$d" && "$CXX" -std=c++17 -I. "_t_${header%.hpp}.cpp" -o "_t_${header%.hpp}" ) >/dev/null 2>&1 || return 1
    ( cd "$d" && "./_t_${header%.hpp}" ) >/dev/null 2>&1
}

# ---- one run ---------------------------------------------------------------
# Runs an instruction once and echoes "<score> <secs>". Isolated HOME so the
# one-shot can't re-root into an active workspace. scorer is a function name that
# takes the temp dir and echoes an integer score.
run_once() {  # <mode> <instruction> <scorer_fn>
    local mode="$1" instr="$2" scorer="$3"
    local tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git commit -q --allow-empty -m init )
    local home="$tmp/home"; mkdir -p "$home"
    local start; start="$(date +%s)"
    if [ "$mode" = single ]; then
        ( cd "$tmp" && HOME="$home" OLLAMA_MODEL="$MODEL" "$CLI" --backend ollama "$instr" >/dev/null 2>&1 )
    else
        ( cd "$tmp" && HOME="$home" OLLAMA_MODEL="$MODEL" "$CLI" crew "$instr" --land auto >/dev/null 2>&1 )
    fi
    local secs=$(( $(date +%s) - start ))
    local score; score="$("$scorer" "$tmp")"
    rm -rf "$tmp"
    echo "$score $secs"
}

# aggregate TRIALS runs -> "passes meanTime stdTime" (passes counts score>=maxscore)
cell() {  # <mode> <instruction> <scorer> <maxscore>
    local mode="$1" instr="$2" scorer="$3" maxscore="$4"
    local passes=0 times=""
    local i sc secs
    for i in $(seq 1 "$TRIALS"); do
        read -r sc secs <<<"$(run_once "$mode" "$instr" "$scorer")"
        [ "${sc:-0}" -ge "$maxscore" ] && passes=$((passes+1))
        times="$times $secs"
    done
    local stats
    stats="$(printf '%s\n' $times | awk '{s+=$1; ss+=$1*$1; n++} END{m=s/n; sd=(n>1)?sqrt((ss-s*s/n)/(n-1)):0; printf "%.0f %.0f", m, sd}')"
    echo "$passes $stats"
}

# ---- Tier A: accuracy on small tasks ---------------------------------------
echo "# Crew vs single agent — publishable benchmark"
echo "model: ${MODEL:-?}   ·   trials/cell: ${TRIALS}   ·   oracle: g++ compile + asserts"
echo
echo "## Tier A — small single-file tasks (crew's worst case)"
printf "%-10s | %-18s | %-18s\n" "task" "single (pass · time)" "crew (pass · time)"
printf -- "-----------+--------------------+--------------------\n"
for row in "${SMALL[@]}"; do
    IFS='|' read -r header instr body <<<"$row"
    _SCORE_HEADER="$header"; _SCORE_BODY="$body"
    scorer_small() { check_one "$1" "$_SCORE_HEADER" "$_SCORE_BODY" && echo 1 || echo 0; }
    read -r sp sm ss <<<"$(cell single "In the current directory, $instr. Define it INLINE in the header (header-only, no separate .cpp). Write the file to disk." scorer_small 1)"
    read -r cp cm cs <<<"$(cell crew   "In the current directory, $instr. Define it INLINE in the header (header-only, no separate .cpp). Write the file to disk." scorer_small 1)"
    printf "%-10s | %-18s | %-18s\n" "${header%.hpp}" "$sp/$TRIALS · ${sm}±${ss}s" "$cp/$TRIALS · ${cm}±${cs}s"
done

# ---- Tier B: parallelism on 4 expensive independent subtasks ----------------
BIG_TASK="In the current directory, create these 4 independent C++ header files, each defining exactly the stated function, each defined INLINE (header-only, no separate .cpp), and write every file to disk:"
for row in "${BIG[@]}"; do
    IFS='|' read -r header sig _ <<<"$row"
    BIG_TASK="$BIG_TASK"$'\n'"- $header defining $sig"
done
scorer_big() {  # counts how many of the 4 compile+pass
    local d="$1" n=0 r h b
    for r in "${BIG[@]}"; do IFS='|' read -r h _ b <<<"$r"; check_one "$d" "$h" "$b" && n=$((n+1)); done
    echo "$n"
}
echo
echo "## Tier B — one task, 4 expensive independent subtasks (crew's best case)"
read -r sp sm ss <<<"$(cell single "$BIG_TASK" scorer_big 4)"
read -r cp cm cs <<<"$(cell crew   "$BIG_TASK" scorer_big 4)"
printf "%-8s | %-22s | %-16s\n" "mode" "all-4 done (of $TRIALS)" "wall-clock"
printf -- "---------+------------------------+-----------------\n"
printf "%-8s | %-22s | %-16s\n" "single" "$sp/$TRIALS" "${sm}±${ss}s"
printf "%-8s | %-22s | %-16s\n" "crew"   "$cp/$TRIALS" "${cm}±${cs}s"
echo
echo "Signal: accuracy parity (compare pass rates ± noise); the robust axis is time."
echo "Crew wins only if Tier B wall-clock is materially LOWER than single — i.e. the"
echo "parallel coders beat one agent doing four expensive subtasks in sequence."
