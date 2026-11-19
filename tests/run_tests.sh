#!/bin/bash
# Regression tests: runs every case in tests/<case>/ and compares the output
# with expected.log / expected.perf.
#   bash tests/run_tests.sh          -> fast, uses sim.out (same engine, no IPC)
#   bash tests/run_tests.sh --ipc    -> full system (process_generator.out, real clock)
cd "$(dirname "$0")/.." || exit 1

mode="sim"
[ "$1" == "--ipc" ] && mode="ipc"

EXPECTED_HEADER="#At time x process y state arr w total z remain y wait k"

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

    # Remove stale outputs before running so a failed run can never match old files.
    rm -f /tmp/scheduler_test.log /tmp/scheduler_test.perf
    if [ "$mode" == "ipc" ]; then
        rm -f scheduler.log scheduler.perf
    fi

    if [ "$mode" == "sim" ]; then
        ./sim.out "$dir/processes.txt" "$dir/resources.txt" "$algorithm" "$quantum" "$switch" \
            /tmp/scheduler_test.log /tmp/scheduler_test.perf
        rc=$?
    else
        ./process_generator.out "$dir/processes.txt" "$dir/resources.txt" \
            "$algorithm" "$quantum" "$switch" > /dev/null
        rc=$?
        if [ -f scheduler.log ]; then cp scheduler.log /tmp/scheduler_test.log; fi
        if [ -f scheduler.perf ]; then cp scheduler.perf /tmp/scheduler_test.perf; fi
    fi

    if [ "$rc" -ne 0 ]; then
        echo "FAIL  $name (exit code $rc)"
        failed=$((failed + 1))
        continue
    fi

    if [ ! -f /tmp/scheduler_test.log ] || [ ! -f /tmp/scheduler_test.perf ]; then
        echo "FAIL  $name (missing output files)"
        failed=$((failed + 1))
        continue
    fi

    # Explicitly check the log header line on generated scheduler.log.
    first_line=$(head -n 1 /tmp/scheduler_test.log)
    if [ "$first_line" != "$EXPECTED_HEADER" ]; then
        echo "FAIL  $name (unexpected header: $first_line)"
        failed=$((failed + 1))
        continue
    fi

    # In --ipc mode, verify no leaked IPC objects or lingering worker/clock processes.
    if [ "$mode" == "ipc" ]; then
        if command -v ipcs >/dev/null 2>&1; then
            leaked_shm=$(ipcs -m | awk 'tolower($1)=="0x0000012c" {print $1}')
            leaked_msg=$(ipcs -q | awk 'tolower($1)=="0x00000190" || tolower($1)=="0x000001f4" {print $1}')
            if [ -n "$leaked_shm" ] || [ -n "$leaked_msg" ]; then
                echo "FAIL  $name (leaked IPC objects: shm=$leaked_shm msg=$leaked_msg)"
                failed=$((failed + 1))
                continue
            fi
        fi
        if command -v pgrep >/dev/null 2>&1; then
            if pgrep -x clk.out >/dev/null 2>&1 || pgrep -x scheduler.out >/dev/null 2>&1 || pgrep -x process.out >/dev/null 2>&1; then
                echo "FAIL  $name (lingering child processes after exit)"
                failed=$((failed + 1))
                continue
            fi
        fi
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
