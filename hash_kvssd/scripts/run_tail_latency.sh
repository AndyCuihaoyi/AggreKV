#!/usr/bin/env bash
# Tail latency benchmark: zipfian read at iodepth=1 across 3 pool sizes.
# Extracts p95/p99/p99.9/p99.99 read latency from logs.
# Results: ./tail_latency_results.csv

set -u
cd "$(dirname "$0")"

ROOT_DIR="$(cd .. && pwd)"
APP_DIR="${ROOT_DIR}/hash_hot_cmt"
BIN="${APP_DIR}/test_ext_mem_lat"
LOG_DIR="${APP_DIR}/tail_latency_logs"
SUMMARY_CSV="${ROOT_DIR}/tail_latency_results.csv"
SEED=1
NR_G=1048576

# NUMA 亲和性：默认 node 0，可通过环境变量 NUMA_NODE 覆盖
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

mkdir -p "${LOG_DIR}"
: > "${SUMMARY_CSV}"

# 表头
echo "pool_gb,map_frac,read_num,iodepth,p95_us,p99_us,p99.9_us,p99.99_us" \
    >> "${SUMMARY_CSV}"

# 配置：pool_gb, map_frac, read_num_per_worker
CONFIGS=(
  "64  0.2500 $((64 * NR_G / 7))"
  "128 0.5000 $((128 * NR_G / 7))"
  "256 1.0000 $((256 * NR_G / 7))"
)

for cfg in "${CONFIGS[@]}"; do
  read -r POOL_GB MAP_FRAC NUM_READ <<< "${cfg}"
  POOL=$((POOL_GB * NR_G))
  RUN_TAG="pool${POOL_GB}G_zipf"
  LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"

  echo "=== ${RUN_TAG} (pool=${POOL_GB}GB, map_frac=${MAP_FRAC}, iodepth=1) ==="

  eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
    --pool_size "${POOL}" \
    --map_size_frac "${MAP_FRAC}" \
    --num_read "${NUM_READ}" \
    --read_dist "zipfian" \
    --read_bench \
    --seed "${SEED}" 2>&1 | tee "${LOG_FILE}"

  echo "--- ${RUN_TAG} done ---"
  echo
done

# 从日志提取尾延迟
echo
echo "========== Extracting tail latencies from logs =========="

for cfg in "${CONFIGS[@]}"; do
  read -r POOL_GB MAP_FRAC NUM_READ <<< "${cfg}"
  RUN_TAG="pool${POOL_GB}G_zipf"
  LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"

  if [[ ! -f "${LOG_FILE}" ]]; then
    echo "WARNING: ${LOG_FILE} not found, skipping"
    continue
  fi

  # 匹配形如 "avg_rlat/us: 123 | p95_rlat/us: 456 | p99_rlat/us: 789 | p99.9_rlat/us: 1234 | p99.99_rlat/us: 5678"
  LINE=$(grep -m1 "avg_rlat/us:" "${LOG_FILE}" || true)
  if [[ -z "${LINE}" ]]; then
    echo "WARNING: no tail latency line found in ${LOG_FILE}"
    echo "${POOL_GB},${MAP_FRAC},${NUM_READ},1,N/A,N/A,N/A,N/A" >> "${SUMMARY_CSV}"
    continue
  fi

  # 用 awk 提取各字段（按 | 分隔，NF=6 → 取 $2 $3 $4 $5）
  # i 是 awk 内部变量，用 -v 传入避免 shell 展开 $i
  echo "${LINE}" | awk -F'|' -v pg="${POOL_GB}" -v mf="${MAP_FRAC}" -v nr="${NUM_READ}" '
  {
    for (i=2; i<=5; i++) gsub(/[^0-9]/, "", $i);
    print pg "," mf "," nr ",1," $2 "," $3 "," $4 "," $5;
  }' | tr -d ' ' >> "${SUMMARY_CSV}"

  echo "Extracted: ${RUN_TAG}"
done

echo
echo "========== Final summary (${SUMMARY_CSV}) =========="
cat "${SUMMARY_CSV}"