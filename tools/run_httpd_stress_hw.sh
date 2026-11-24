#!/bin/bash
# Full hardware stress test suite for Muzix httpd.
#
# Runs against real hardware at 192.168.0.50:80 (LAN) and/or
# 93.177.187.134:1976 (external, port-forwarded).
#
# For each of 3 paths (/cgi-bin/hello, /, /nonexistent-path),
# runs 3 independent iterations of 200 requests each.
#
# Usage:
#   tools/run_httpd_stress_hw.sh [--lan | --external | --both]
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STRESS="$ROOT/tools/httpd_stress_hw.py"

MODE="${1:-both}"

PATHS=("/cgi-bin/hello" "/" "/nonexistent-path")
ITERATIONS=200
REPEATS=3

results_file="/tmp/httpd_stress_hw_results_$(date +%Y%m%d_%H%M%S).txt"
{
    echo "========================================================================"
    echo "Muzix httpd hardware stress test suite"
    echo "Date: $(date)"
    echo "Mode: $MODE"
    echo "Iterations per run: $ITERATIONS"
    echo "Repeats per path: $REPEATS"
    echo "========================================================================"
    echo ""
} | tee "$results_file"

fail_count=0

for target_mode in $MODE; do
    case "$target_mode" in
        lan)
            HOST="192.168.0.50"
            PORT=80
            ;;
        external)
            HOST="93.177.187.134"
            PORT=1976
            ;;
        both)
            # Will loop both below
            ;;
        *)
            echo "Unknown mode: $target_mode (use lan, external, or both)" >&2
            exit 2
            ;;
    esac

    targets=()
    if [ "$target_mode" = "both" ]; then
        targets+=("192.168.0.50 80 LAN")
        targets+=("93.177.187.134 1976 EXTERNAL")
    else
        targets+=("$HOST $PORT $(echo $target_mode | tr '[:lower:]' '[:upper:]')")
    fi

    for target in "${targets[@]}"; do
        read -r t_host t_port t_label <<< "$target"

        echo "" | tee -a "$results_file"
        echo "========================================================================" | tee -a "$results_file"
        echo "Target: $t_label ($t_host:$t_port)" | tee -a "$results_file"
        echo "========================================================================" | tee -a "$results_file"

        for path in "${PATHS[@]}"; do
            echo "" | tee -a "$results_file"
            echo "--- Path: $path ---" | tee -a "$results_file"

            for rep in $(seq 1 $REPEATS); do
                echo "" | tee -a "$results_file"
                echo "Repeat $rep/$REPEATS: $ITERATIONS requests to $path" | tee -a "$results_file"

                # Run the stress test
                output=$(python3 "$STRESS" \
                    --host "$t_host" \
                    --port "$t_port" \
                    --path "$path" \
                    --iterations "$ITERATIONS" \
                    --scenarios a \
                    --wait-on-fail 2>&1) || true

                echo "$output" | tee -a "$results_file"

                # Check for first failure
                first_fail=$(echo "$output" | grep -m1 "SERVER STOPPED RESPONDING\|CONNECT FAILED\|SCENARIO FAILED\|WATCHDOG" || true)
                if [ -n "$first_fail" ]; then
                    echo ">>> FIRST FAILURE: $first_fail" | tee -a "$results_file"
                    fail_count=$((fail_count + 1))
                else
                    echo ">>> All $ITERATIONS requests OK" | tee -a "$results_file"
                fi

                # If we had a failure, wait before next repeat to let hardware recover
                if [ -n "$first_fail" ]; then
                    echo ">>> Waiting 30 s for hardware to recover before next repeat..." | tee -a "$results_file"
                    sleep 30
                fi
            done
        done
    done
done

echo "" | tee -a "$results_file"
echo "========================================================================" | tee -a "$results_file"
echo "SUMMARY" | tee -a "$results_file"
echo "Total failures detected: $fail_count" | tee -a "$results_file"
echo "Results saved to: $results_file" | tee -a "$results_file"
echo "========================================================================" | tee -a "$results_file"

exit $fail_count
