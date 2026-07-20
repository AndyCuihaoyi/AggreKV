#!/usr/bin/env bash
# GC wear benchmarking: update workload to trigger GC, then collect
# per-sblk erase counts from logs.
# Single config: pool_size=256GB, update_num=32GB, zipfian.
# Output: tail_latency_logs/pool256G_update/gc_wear.log

set -u
cd "$(dirname "$0")"

ROOT_DIR="$(cd .. && pwd)"
APP_DIR="${ROOT_DIR}/hash_hot_cmt"
BIN="${APP_DIR}/test_ext_mem_lat"
LOG_DIR="${APP_DIR}/gc_wear_logs"
SEED=1
NR_G=1048576

# NUMA 亲和性：默认 node 0（192GB），可通过环境变量 NUMA_NODE 覆盖
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

# 配置：pool_size=256GB, update_num=32GB, zipfian, map_frac=256/256=1.0
POOL=$((10 * NR_G))
POOL_GB=10
UPDATE_NUM=$((10 * NR_G))  #每个worker的更新量为10GB
MAP_FRAC=1.0000

RUN_TAG="pool${POOL_GB}G_update${UPDATE_NUM}_zipfian"
LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"
SUMMARY_CSV="${ROOT_DIR}/gc_wear_results.csv"

mkdir -p "${LOG_DIR}"

echo "=== ${RUN_TAG} (pool=${POOL_GB}GB pages, update=${UPDATE_NUM} pages, zipfian) ==="

eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
  --pool_size "${POOL}" \
  --map_size_frac "${MAP_FRAC}" \
  --num_update "${UPDATE_NUM}" \
  --update_dist "zipfian" \
  --update_bench \
  --ssd_lat_off \
  --seed "${SEED}" 2>&1 | tee "${LOG_FILE}"

echo
echo "========== Extracting GC counts from log =========="

# 提取所有 "sblk[N]: gc_cnt=X" 行
grep "sblk\[" "${LOG_FILE}" > "${LOG_DIR}/${RUN_TAG}_sblk.gc"

# 提取 total
TOTAL=$(grep "total GC erase count:" "${LOG_FILE}" | sed 's/.*: //')
echo "Total GC count: ${TOTAL}"

# 写入 CSV
: > "${SUMMARY_CSV}"
echo "sblk_index,gc_cnt" >> "${SUMMARY_CSV}"
grep "sblk\[" "${LOG_FILE}" | sed 's/.*sblk\[\([0-9]*\)\]: gc_cnt=\([0-9]*\).*/\1,\2/' >> "${SUMMARY_CSV}"

echo
echo "========== GC count per SBLK (${SUMMARY_CSV}) =========="
cat "${SUMMARY_CSV}"
