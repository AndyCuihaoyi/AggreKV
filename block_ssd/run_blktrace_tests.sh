#!/bin/bash
# Run the blktrace analysis test with the specified blktrace file and parameters.
# This script assumes that the test_blktrace binary is located in the dftl_block directory.

# --- Guard: blktrace dataset must be present ---
# In the standard flow (downloading the Zenodo archive), the blktrace traces
# are already unpacked at block_ssd/blktrace/ and this guard does not fire.
# It exists only as a safety net for partial checkouts (e.g. cloning the
# source tree without the Zenodo archive). In that case we exit 0 so the
# overall `bash test.sh` can still complete.
if [[ ! -d "./blktrace" ]] || [[ -z "$(ls -A "./blktrace" 2>/dev/null)" ]]; then
    echo "[block_ssd] blktrace dataset missing under ./blktrace/" >&2
    echo "[block_ssd] To run the Block-SSD baseline, fetch the Zenodo archive and extract it; the blktrace traces will land at block_ssd/blktrace/." >&2
    echo "[block_ssd] Skipping Block-SSD baseline (Functional badge does not require this step)." >&2
    exit 0
fi

BLKTRACE_DIR=./blktrace
RESULT_DIR=./blktrace_test_results
SUMMARY_DIR=./blktrace_summary

mkdir -p "$RESULT_DIR" "$SUMMARY_DIR"

for blktrace_file in $BLKTRACE_DIR/*.csv; do
    cnt_file="${blktrace_file%.csv}.cnt"
    if [[ ! -f "$cnt_file" ]]; then
        echo "Missing cnt file for $blktrace_file: $cnt_file" >&2
        continue
    fi

    num_upper_level_kvops=$(tr -d '[:space:]' < "$cnt_file")
    if [[ ! "$num_upper_level_kvops" =~ ^[0-9]+$ ]]; then
        echo "Invalid cnt file content in $cnt_file: $num_upper_level_kvops" >&2
        continue
    fi

    echo "Running blktrace analysis for file: $blktrace_file"
    echo "num_upper_level_kvops: $num_upper_level_kvops"
    ./dftl_block/test_blktrace --blktrace_file "$blktrace_file" --num_upper_level_kvops "$num_upper_level_kvops" 2>&1 | tee ./$RESULT_DIR/tmp.test_$(basename "$blktrace_file").log
    mv "./$RESULT_DIR/tmp.test_$(basename "$blktrace_file").log" "./$RESULT_DIR/test_$(basename "$blktrace_file").log"
done

# Extract summary from logs
echo ""
echo "=========================================="
echo "Extracting summary results..."
echo "=========================================="

SUMMARY_FILE="$SUMMARY_DIR/summary_$(date +%Y%m%d_%H%M%S).txt"
: > "$SUMMARY_FILE"

for log_file in $RESULT_DIR/test_*.log; do
    if [[ ! -f "$log_file" ]]; then
        continue
    fi

    test_name=$(basename "$log_file" .log)
    echo "" >> "$SUMMARY_FILE"
    echo "========================================" >> "$SUMMARY_FILE"
    echo "Test: $test_name" >> "$SUMMARY_FILE"
    echo "========================================" >> "$SUMMARY_FILE"

    # Extract configuration info
    echo "" >> "$SUMMARY_FILE"
    echo "[Configuration]" >> "$SUMMARY_FILE"
    grep -E "ssd created|GRAINED_UNIT|hash_table_size|cached:" "$log_file" | head -5 >> "$SUMMARY_FILE"
    grep -E "blktrace records:|replay requests:" "$log_file" | head -2 >> "$SUMMARY_FILE"
    grep -E "SSD Latency|workload size" "$log_file" | head -2 >> "$SUMMARY_FILE"

    # Extract final results
    echo "" >> "$SUMMARY_FILE"
    echo "[Final Results]" >> "$SUMMARY_FILE"
    grep "average request latency" "$log_file" >> "$SUMMARY_FILE"
    grep "finished_total" "$log_file" >> "$SUMMARY_FILE"
    grep "data_rd:\|mapping_rd:\|mapping_wr:" "$log_file" >> "$SUMMARY_FILE"
    grep "nand_r:\|nand_w:\|nand_e:" "$log_file" >> "$SUMMARY_FILE"
    grep "total iops" "$log_file" >> "$SUMMARY_FILE"
    grep "upper level kvops" "$log_file" >> "$SUMMARY_FILE"
    grep "d_cache_hit\|miss:" "$log_file" | head -2 >> "$SUMMARY_FILE"
    grep "CMT number" "$log_file" >> "$SUMMARY_FILE"
    grep "avg_hit_inpage" "$log_file" >> "$SUMMARY_FILE"

    # Extract tail latency stats
    echo "" >> "$SUMMARY_FILE"
    echo "[Tail Latency]" >> "$SUMMARY_FILE"
    tail -10 "$log_file" >> "$SUMMARY_FILE"

    echo "Extracted: $test_name"
done

echo ""
echo "=========================================="
echo "Summary saved to: $SUMMARY_FILE"
echo "=========================================="

# Also create a CSV version for easy analysis
CSV_FILE="$SUMMARY_DIR/summary_$(date +%Y%m%d_%H%M%S).csv"
echo "test_name,total_iops,avg_lat_us,total_requests,hit_rate,nand_read,nand_write,upper_kvops" > "$CSV_FILE"

for log_file in $RESULT_DIR/test_*.log; do
    if [[ ! -f "$log_file" ]]; then
        continue
    fi

    test_name=$(basename "$log_file" .log | sed 's/test_//')

    # Extract values with sed
    total_iops=$(grep "total iops" "$log_file" | sed 's/.*total iops: \([0-9.]*\).*/\1/')
    avg_lat=$(grep "average request latency" "$log_file" | sed 's/.*latency: \([0-9]*\) us.*/\1/')
    total_req=$(grep "finished_total" "$log_file" | sed 's/.*finished_total: \([0-9]*\),.*/\1/')
    hit_rate=$(grep "total iops" "$log_file" | sed 's/.*hit rt:\([0-9.]*\)%.*/\1/')
    nand_r=$(grep "nand_r:" "$log_file" | sed 's/.*nand_r: \([0-9]*\),.*/\1/')
    nand_w=$(grep "nand_w:" "$log_file" | sed 's/.*nand_w: \([0-9]*\),.*/\1/')
    upper_kvops=$(grep "upper level kvops" "$log_file" | sed 's/.*kvops: \([0-9.]*\).*/\1/')

    echo "$test_name,$total_iops,$avg_lat,$total_req,$hit_rate,$nand_r,$nand_w,$upper_kvops" >> "$CSV_FILE"
done

echo "CSV summary saved to: $CSV_FILE"
