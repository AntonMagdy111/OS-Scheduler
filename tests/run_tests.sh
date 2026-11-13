#!/bin/bash
# Regression tests: runs every case in tests/<case>/ and compares the output
# with expected.log / expected.perf.
#   bash tests/run_tests.sh          -> fast, uses sim.out (same engine, no IPC)
#   bash tests/run_tests.sh --ipc    -> full system (process_generator.out, real clock)
cd "$(dirname "$0")/.." || exit 1

mode="sim"
[ "$1" == "--ipc" ] && mode="ipc"

passed=0
failed=0
for dir in tests/*/; do
    name=$(basename "$dir")
    [ -f "$dir/params" ] || continue
    read -r algorithm quantum switch < "$dir/params"

    if [ "$mode" == "sim" ]; then
        ./sim.out "$dir/processes.txt" "$dir/resources.txt" "$algorithm" "$quantum" "$switch" \
            /tmp/scheduler_test.log /tmp/scheduler_test.perf
    else
        ./process_generator.out "$dir/processes.txt" "$dir/resources.txt" \
            "$algorithm" "$quantum" "$switch" > /dev/null
        cp scheduler.log /tmp/scheduler_test.log
        cp scheduler.perf /tmp/scheduler_test.perf
    fi

    if diff -q "$dir/expected.log" /tmp/scheduler_test.log > /dev/null &&
       diff -q "$dir/expected.perf" /tmp/scheduler_test.perf > /dev/null; then
        echo "PASS  $name"
        passed=$((passed + 1))
    else
        echo "FAIL  $name"
        diff "$dir/expected.log" /tmp/scheduler_test.log
        diff "$dir/expected.perf" /tmp/scheduler_test.perf
        failed=$((failed + 1))
    fi
done
echo "Passed: $passed  Failed: $failed"
[ "$failed" -eq 0 ]
