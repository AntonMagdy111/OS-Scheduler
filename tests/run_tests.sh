#!/bin/bash
# Regression tests: runs every case in tests/<case>/ and compares the output
# with expected.log / expected.perf (ignoring '#' comment lines).
#   bash tests/run_tests.sh          -> fast, uses sim.out (same engine, no IPC)
#   bash tests/run_tests.sh --ipc    -> full system (process_generator.out, real clock)
cd "$(dirname "$0")/.." || exit 1

mode="sim"
[ "$1" == "--ipc" ] && mode="ipc"

passed=0
failed=0
for dir in tests/*/; do
    name=$(basename "$dir")
    if [ -f "$dir/params" ]; then
        read -r algorithm quantum switch < "$dir/params"
    elif [ -f "$dir/args" ]; then
        read -r algorithm quantum switch < "$dir/args"
    else
        continue
    fi
    [ -z "$quantum" ] && quantum=1
    [ -z "$switch" ] && switch=1

    if [ "$mode" == "sim" ]; then
        ./sim.out "$dir/processes.txt" "$dir/resources.txt" "$algorithm" "$quantum" "$switch" \
            /tmp/scheduler_test.log /tmp/scheduler_test.perf
    else
        ./process_generator.out "$dir/processes.txt" "$dir/resources.txt" \
            "$algorithm" "$quantum" "$switch" > /dev/null
        cp scheduler.log /tmp/scheduler_test.log
        cp scheduler.perf /tmp/scheduler_test.perf
    fi

    grep -v '^#' "$dir/expected.log" > /tmp/expected_nocomment.log
    grep -v '^#' /tmp/scheduler_test.log > /tmp/actual_nocomment.log

    if diff -q /tmp/expected_nocomment.log /tmp/actual_nocomment.log > /dev/null &&
       diff -q "$dir/expected.perf" /tmp/scheduler_test.perf > /dev/null; then
        echo "PASS  $name"
        passed=$((passed + 1))
    else
        echo "FAIL  $name"
        diff /tmp/expected_nocomment.log /tmp/actual_nocomment.log
        diff "$dir/expected.perf" /tmp/scheduler_test.perf
        failed=$((failed + 1))
    fi
done
echo "Passed: $passed  Failed: $failed"
[ "$failed" -eq 0 ]
