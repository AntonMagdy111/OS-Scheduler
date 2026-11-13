#!/bin/bash
# usage: run_case.sh <case dir> [tick_us]   -- runs one case in a scratch dir and diffs the outputs
# tick_us=1000000 uses the real clk.out from the project; anything else uses fast_clk.
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
CASE="$(cd "$1" && pwd)"
TICK="${2:-1000000}"
RUN="$HERE/run/$(basename "$CASE")"
rm -rf "$RUN"; mkdir -p "$RUN"
cp "$ROOT"/{process_generator.out,scheduler.out,process.out} "$RUN/"
if [ "$TICK" = "1000000" ]; then cp "$ROOT/clk.out" "$RUN/clk.out"; else cp "$HERE/fast_clk.out" "$RUN/clk.out"; fi
cp "$CASE/processes.txt" "$CASE/resources.txt" "$RUN/" 2>/dev/null
cd "$RUN"
TICK_US=$TICK timeout 600 ./process_generator.out $(cat "$CASE/args") > stdout.txt 2> stderr.txt
echo "exit=$?" > status.txt
if [ -f "$CASE/expected.log" ]; then
  diff <(grep -v '^#' "$CASE/expected.log") <(grep -v '^#' scheduler.log) > log.diff && diff "$CASE/expected.perf" scheduler.perf > perf.diff
  if [ $? -eq 0 ]; then echo "PASS $(basename "$CASE")"; else echo "FAIL $(basename "$CASE")"; cat log.diff perf.diff 2>/dev/null | head -20; exit 1; fi
fi
