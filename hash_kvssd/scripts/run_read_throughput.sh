#!/usr/bin/env bash
# read-only benchmark: 3 pool sizes × 2 read distributions = 6 runs.
# Each run is a single-process invocation of test_ext_mem_lat --read_bench.
# Results are appended to ./read_test_summary.csv (one line per run).
#
# 二进制位于 hash_hot_cmt/，CSV/日志统一写到 hash_hot_cmt/ 下，
# 这样无论从哪个目录调用本脚本都能正确找到二进制与产物。

set -u
cd "$(dirname "$0")"

# 仓库根目录 = scripts/ 的父目录
ROOT_DIR="$(cd .. && pwd)"
APP_DIR="${ROOT_DIR}/hash_hot_cmt"
BIN="${APP_DIR}/test_ext_mem_lat"
SEED=1
LOG_DIR="${APP_DIR}/read_bench_logs"
SUMMARY_CSV="${APP_DIR}/read_test_summary.csv"

# NUMA 亲和性：默认 node 0，可通过环境变量 NUMA_NODE 覆盖
#   例如：NUMA_NODE=1 bash run_read_throughput.sh
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

mkdir -p "${LOG_DIR}"
# 每次重新清空汇总文件，避免历史结果污染
: > "${SUMMARY_CSV}"

# 容量配置：pool_size, pool_size_in_GB, read_num_in_GB
# pool_size 单位：keys；nr_G_workload=1048576 → 64G=64*1048576=67108864
# map_size_frac 必须是 (0, 1] 的浮点（原 main 中 < 1 才生效，
# 因此 64/128/256 都不应直接传整数）。这里用 num/256 计算。
NR_G=1048576
SIZES=(
  "$((64 * NR_G))  64  64"   # 64G  (map_sizefrac=64/256=0.25)
  "$((128 * NR_G)) 128 128"  # 128G (map_sizefrac=128/256=0.50)
  "$((256 * NR_G)) 256 256"  # 256G (map_sizefrac=256/256=1.00)
)
DISTS=("uniform" "zipfian")

for entry in "${SIZES[@]}"; do
  read -r POOL POOL_G READ_G <<< "${entry}"
  # 显式以浮点字符串传入，避免被 main 端的 atoi 当作整数截断
  FRAC=$(awk -v n="${POOL_G}" 'BEGIN{printf "%.4f", n/256}')
  for DIST in "${DISTS[@]}"; do
    RUN_TAG="pool${POOL_G}G_${DIST}"
    LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"
    echo "=== Run ${RUN_TAG} (pool_size=${POOL}, map_sizefrac=${FRAC}, dist=${DIST}) ==="
    eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
      --pool_size "${POOL}" \
      --map_size_frac "${FRAC}" \
      --num_read $((READ_G * NR_G / 7)) \
      --read_dist "${DIST}" \
      --read_bench \
      --seed "${SEED}" 2>&1 | tee "${LOG_FILE}"
    echo "--- ${RUN_TAG} done; summary appended to ${SUMMARY_CSV} ---"
  done
done

echo
echo "========== Final summary (${SUMMARY_CSV}) =========="
cat "${SUMMARY_CSV}"
