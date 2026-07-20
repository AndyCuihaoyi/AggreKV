#!/usr/bin/env bash
# update-only benchmark: 3 pool sizes × 2 update distributions = 6 runs.
# Each run is a single-process invocation of test_ext_mem_lat --update_bench.
# Results are appended to ./update_test_summary.csv (one line per run).
#
# update_num 固定为 64GB（64M keys），不随 pool size 变化。

set -u
cd "$(dirname "$0")"

ROOT_DIR="$(cd .. && pwd)"
APP_DIR="${ROOT_DIR}/hash_hot_cmt"
BIN="${APP_DIR}/test_ext_mem_lat"
SEED=1
LOG_DIR="${APP_DIR}/update_bench_logs"
SUMMARY_CSV="${APP_DIR}/update_test_summary.csv"
NR_G=1048576

# NUMA 亲和性：默认 node 0，可通过环境变量 NUMA_NODE 覆盖
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

# update_num 固定 64GB = 64M keys（不随 pool size 变化）
UPDATE_NUM=$((16 * NR_G))

mkdir -p "${LOG_DIR}"
: > "${SUMMARY_CSV}"

# pool_size, pool_size_in_GB, map_frac
# map_frac 必须是 (0, 1] 的浮点（main 中 map_size_frac < 1 才缩容）
CONFIGS=(
  "$((64  * NR_G))  64  0.2500"   # 64G  → map_sizefrac=64/256=0.25
  "$((128 * NR_G))  128 0.5000"   # 128G → map_sizefrac=128/256=0.50
  "$((256 * NR_G))  256 1.0000"   # 256G → map_sizefrac=256/256=1.00
)
DISTS=("uniform" "zipfian")

for cfg in "${CONFIGS[@]}"; do
  read -r POOL POOL_GB MAP_FRAC <<< "${cfg}"
  for DIST in "${DISTS[@]}"; do
    RUN_TAG="pool${POOL_GB}G_${DIST}"
    LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"
    echo "=== Run ${RUN_TAG} (pool_size=${POOL_GB}GB, map_frac=${MAP_FRAC}, "
    echo "              update_num=${UPDATE_NUM}, dist=${DIST}) ==="
    eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
      --pool_size "${POOL}" \
      --map_size_frac "${MAP_FRAC}" \
      --num_update "${UPDATE_NUM}" \
      --update_dist "${DIST}" \
      --update_bench \
      --seed "${SEED}" 2>&1 | tee "${LOG_FILE}"
    echo "--- ${RUN_TAG} done ---"
    echo
  done
done

echo
echo "========== Final summary (${SUMMARY_CSV}) =========="
cat "${SUMMARY_CSV}"