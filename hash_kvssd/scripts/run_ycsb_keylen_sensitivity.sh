#!/bin/bash
# YCSB Workload A key-length sensitivity benchmark.
# Tests throughput (AvgIOPS) across 3 key lengths: 16B / 32B / 64B.
#
# Fixed config: pool_size=128GB, YCSB_a ops=80M, zipfian.
# Each key-length is a fresh build → load → YCSB_A run.
#
# Usage:
#   bash scripts/run_ycsb_keylen_sensitivity.sh
#   NUMA_NODE=1 bash scripts/run_ycsb_keylen_sensitivity.sh   # bind to specific NUMA node

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

RESULTS_DIR="ycsb_keylen_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
SUMMARY_FILE="${RESULTS_DIR}/summary.txt"

# 固定配置
POOL_SIZE=67108864     # 64GB
MAP_SIZE_FRAC=0.2500    # 64/256
YCSB_OPS=80000000      # 80M ops
KEYLEN_ARRAY=(16 32 64)

# NUMA 亲和性
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

echo "============================================"
echo " YCSB Workload A — Key-Length Sensitivity"
echo "============================================"
echo " Pool:       ${POOL_SIZE} items (64GB)"
echo " MapFrac:    ${MAP_SIZE_FRAC} (64/256)"
echo " YCSB ops:  ${YCSB_OPS}"
echo " KeyLens:    ${KEYLEN_ARRAY[*]}B"
echo " NUMA node:  ${NUMA_NODE}"
echo " Results:    ${RESULTS_DIR}/"
echo "============================================"
echo ""

# ---------- 表头 ----------
printf "| %-8s | %-12s | %-12s | %-12s | %-12s |\n" \
    "KeyLen" "TotalOps" "AvgIOPS" "AvgLat(us)" "HitRate(%)" > "${SUMMARY_FILE}"
printf "| %-8s | %-12s | %-12s | %-12s | %-12s |\n" \
    "--------" "------------" "------------" "------------" "------------" >> "${SUMMARY_FILE}"

# ---------- 编译一次 ----------
echo "[$(date '+%H:%M:%S')] make clean..."
make clean 2>&1 | tail -1
echo "[$(date '+%H:%M:%S')] make test_ext_mem_lat..."
make test_ext_mem_lat 2>&1 | tail -3
BINARY="hash_hot_cmt/test_ext_mem_lat"
if [ ! -f "${BINARY}" ]; then
    echo "ERROR: Binary not found at ${BINARY}"
    exit 1
fi
echo ""

# ---------- 按 keylen 逐个跑 ----------
for KL in "${KEYLEN_ARRAY[@]}"; do
    RUN_TAG="keylen${KL}B"
    LOG_FILE="${RESULTS_DIR}/ycsb_a_${RUN_TAG}.log"

    echo "============================================"
    echo " [$(date '+%H:%M:%S')] keylen=${KL}B start"
    echo "============================================"

    # 每次重新 load（load 阶段不受 variable_keylen 影响，但确保状态干净）
    if ! eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" ./"${BINARY}" \
        --pool_size "${POOL_SIZE}" \
        --map_size_frac "${MAP_SIZE_FRAC}" \
        --ycsb a \
        --ycsb_ops "${YCSB_OPS}" \
        --keylen_mean "${KL}" \
        --variable_keylen \
        > "${LOG_FILE}" 2>&1; then
        echo "ERROR: keylen=${KL}B run failed (see ${LOG_FILE})"
        printf "| %-8s | %-12s | %-12s | %-12s | %-12s |\n" \
            "${KL}B" "FAILED" "-" "-" "-" >> "${SUMMARY_FILE}"
        continue
    fi

    # 提取关键指标
    TOTAL_OPS=$(grep -oP 'Total ops:\s*\K[\d.]+' "${LOG_FILE}" | head -1)
    AVG_IOPS=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "${LOG_FILE}" | head -1)
    AVG_LAT=$(grep -oP 'Avg latency:\s*\K[\d.]+' "${LOG_FILE}" | head -1)
    MAX_LAT=$(grep -oP 'Max latency:\s*\K[\d.]+' "${LOG_FILE}" | head -1)
    HIT_RATE=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "${LOG_FILE}" | head -1)

    printf "| %-8s | %-12s | %-12s | %-12s | %-12s |\n" \
        "${KL}B" \
        "${TOTAL_OPS:-N/A}" \
        "${AVG_IOPS:-N/A}" \
        "${AVG_LAT:-N/A}" \
        "${HIT_RATE:-N/A}" >> "${SUMMARY_FILE}"

    echo ""
    echo "  keylen=${KL}B done."
    echo "  IOPS:     ${AVG_IOPS:-N/A}"
    echo "  Avg Lat:  ${AVG_LAT:-N/A} us"
    echo "  Hit Rate: ${HIT_RATE:-N/A}%"
    echo ""
done

# ---------- 结束 ----------
printf "--------------------------------------------------------\n" >> "${SUMMARY_FILE}"
printf "Finished at: %s\n" "$(date)" >> "${SUMMARY_FILE}"

echo "============================================"
echo " YCSB Workload A — Key-Length Sensitivity DONE"
echo "============================================"
echo ""
echo "Results dir: ${RESULTS_DIR}/"
echo ""
echo "=== Summary ==="
cat "${SUMMARY_FILE}"
