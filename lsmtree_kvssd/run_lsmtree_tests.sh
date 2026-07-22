#!/bin/bash
#
# LSM-tree Test Script
# Runs multiple workloads and saves results to lsmtree_test_results folder
#
# Usage: bash run_lsmtree_tests.sh [--num N]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Default configuration
NUM_KV=8000000

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --num)
            NUM_KV="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: bash run_lsmtree_tests.sh [--num N]"
            exit 1
            ;;
    esac
done

# Create results directory
RESULTS_DIR="lsmtree_test_results"
mkdir -p "$RESULTS_DIR"

# Workloads to test
WORKLOADS=(random_read zipfian_read zipfian_update ycsb_a ycsb_b ycsb_c ycsb_d ycsb_e ycsb_f)

echo "========================================"
echo " LSM-tree Test Suite"
echo "========================================"
echo " Results directory: $RESULTS_DIR"
echo " KV pairs:         $NUM_KV"
echo " Workloads:        ${WORKLOADS[*]}"
echo "========================================"
echo ""

# Clean and build
echo "[$(date '+%H:%M:%S')] Cleaning previous build..."
make clean > /dev/null 2>&1

echo "[$(date '+%H:%M:%S')] Building lsm_test..."
if ! make lsm_test 2>&1 | tail -5; then
    echo "[ERROR] Build failed!"
    exit 1
fi

if [ ! -f "./lsm_test" ]; then
    echo "[ERROR] Binary not found at ./lsm_test"
    exit 1
fi

echo "[$(date '+%H:%M:%S')] Build completed successfully"
echo ""

# Summary file
SUMMARY_FILE="$RESULTS_DIR/summary.txt"
cat > "$SUMMARY_FILE" << EOF
========================================
LSM-tree Test Summary
========================================
Date: $(date)
KV pairs: $NUM_KV
========================================
Workload        TotalOps    AvgIOPS    MaxLat(us)
--------       --------    -------    ----------
EOF

# Run each workload
for workload in "${WORKLOADS[@]}"; do
    echo "========================================"
    echo "[$(date '+%H:%M:%S')] Running workload: $workload"
    echo "========================================"

    LOG_FILE="$RESULTS_DIR/${workload}.log"
    START_TIME=$(date +%s)

    # Run test and capture output
    ./lsm_test --workload "$workload" --num "$NUM_KV" > "$LOG_FILE" 2>&1

    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    echo "[$(date '+%H:%M:%S')] Completed in ${ELAPSED}s"
    echo ""

    # Extract metrics from log
    if grep -q "\[READ STAT\]" "$LOG_FILE"; then
        TOTAL_OPS=$(grep "\[READ STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*\[READ STAT\] \([0-9]*\) th read end.*/\1/')
        AVG_IOPS=$(grep "\[READ STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*total_avg_iops: \([0-9.]*\).*/\1/')
        MAX_LAT=$(grep "\[READ STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*max_lat: \([0-9]*\) us.*/\1/')
        [ -z "$TOTAL_OPS" ] && TOTAL_OPS="N/A"
        [ -z "$AVG_IOPS" ] && AVG_IOPS="N/A"
        [ -z "$MAX_LAT" ] && MAX_LAT="N/A"
    elif grep -q "\[WRITE STAT\]" "$LOG_FILE"; then
        TOTAL_OPS=$(grep "\[WRITE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*\[WRITE STAT\] \([0-9]*\) th write end.*/\1/')
        AVG_IOPS=$(grep "\[WRITE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*total_avg_iops: \([0-9.]*\).*/\1/')
        MAX_LAT=$(grep "\[WRITE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*max_lat: \([0-9]*\) us.*/\1/')
        [ -z "$TOTAL_OPS" ] && TOTAL_OPS="N/A"
        [ -z "$AVG_IOPS" ] && AVG_IOPS="N/A"
        [ -z "$MAX_LAT" ] && MAX_LAT="N/A"
    elif grep -q "\[UPDATE STAT\]" "$LOG_FILE"; then
        TOTAL_OPS=$(grep "\[UPDATE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*\[UPDATE STAT\] \([0-9]*\) th update end.*/\1/')
        AVG_IOPS=$(grep "\[UPDATE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*total_avg_iops: \([0-9.]*\).*/\1/')
        MAX_LAT=$(grep "\[UPDATE STAT\]" "$LOG_FILE" | tail -1 | sed 's/.*max_lat: \([0-9]*\) us.*/\1/')
        [ -z "$TOTAL_OPS" ] && TOTAL_OPS="N/A"
        [ -z "$AVG_IOPS" ] && AVG_IOPS="N/A"
        [ -z "$MAX_LAT" ] && MAX_LAT="N/A"
    else
        TOTAL_OPS="N/A"
        AVG_IOPS="N/A"
        MAX_LAT="N/A"
    fi

    printf "%-15s %10s %10s %12s\n" \
        "$workload" "${TOTAL_OPS:-N/A}" "${AVG_IOPS:-N/A}" "${MAX_LAT:-N/A}" >> "$SUMMARY_FILE"

    echo "  Ops: $TOTAL_OPS, IOPS: $AVG_IOPS, Max: ${MAX_LAT}us"
    echo ""
done

# Add completion info
cat >> "$SUMMARY_FILE" << EOF
========================================
Finished at: $(date)
========================================
EOF

echo "========================================"
echo " All tests completed!"
echo "========================================"
echo ""
echo "Results directory: $RESULTS_DIR/"
echo ""
echo "=== Summary ==="
cat "$SUMMARY_FILE"
