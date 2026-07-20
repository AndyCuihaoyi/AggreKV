#!/usr/bin/env bash
# Keylen Sensitivity Analysis: zipf update workload
# Pool=64GB, map_frac=64/256 & 256/256, update_num=64M
# 输出: keylen_sensitivity_results.csv

set -uuo pipefail
cd "$(dirname "$0")"

ROOT_DIR="$(cd .. && pwd)"
APP_DIR="${ROOT_DIR}/hash_hot_cmt"
BIN="${APP_DIR}/test_ext_mem_lat"
LOG_DIR="${APP_DIR}/keylen_sensitivity_logs"
NR_G=1048576
SEED=1
RESULTS_CSV="${ROOT_DIR}/scripts/keylen_sensitivity.csv"

# 配置
POOL_SIZE=$((64 * NR_G))       # 64GB
POOL_GB=64
UPDATE_NUM=$((64 * NR_G))      # 64M
MAP_FRACS=(0.2500)      # 64/256=0.25
KEYLENS=(16 32 64)             # 16B, 32B, 64B

mkdir -p "${LOG_DIR}"

echo "============================================"
echo " Keylen Sensitivity (zipf update)"
echo " Pool=${POOL_GB}GB  update_num=64M"
echo " map_frac: 64/256=0.2500"
echo " keylen:   16B  32B  64B"
echo "============================================"

# NUMA 亲和性
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

# ----- 初始化 CSV -----
# 表头: scheme,16B AvgIOPS,32B AvgIOPS,64B AvgIOPS
: > "${RESULTS_CSV}"
echo "scheme,   16B AvgIOPS,    32B AvgIOPS,    64B AvgIOPS" >> "${RESULTS_CSV}"

# ----- 按 map_frac 分组运行 -----
# 每个 map_frac 用独立空白 Pool，跑完三个 keylen 后换下一个 map_frac
for MAP_FRAC in "${MAP_FRACS[@]}"; do
    MAP_FRAC_LABEL=$(echo "${MAP_FRAC}" | awk '{printf "map%dp%d", int($1*256), 256}')
    echo ""
    echo "[$(date '+%H:%M:%S')] ===== map_frac=${MAP_FRAC} (${MAP_FRAC_LABEL}) ====="

    # 临时数组：收集当前 map_frac 下各 keylen 的 IOPS
    declare -a IOPS_16 IOPS_32 IOPS_64

    for KEYLEN in "${KEYLENS[@]}"; do
        RUN_TAG="pool${POOL_GB}G_keylen${KEYLEN}_${MAP_FRAC_LABEL}_zipf"
        LOG_FILE="${LOG_DIR}/${RUN_TAG}.log"

        echo "[$(date '+%H:%M:%S')] --- keylen=${KEYLEN}B ---"

        # 编译（如需）
        if [ ! -f "${BIN}" ]; then
            echo "[$(date '+%H:%M:%S')] make test_ext_mem_lat..."
            make -C "${ROOT_DIR}" test_ext_mem_lat 2>&1 | tail -3
        fi

        eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
            --pool_size "${POOL_SIZE}" \
            --map_size_frac "${MAP_FRAC}" \
            --num_update "${UPDATE_NUM}" \
            --update_dist "zipfian" \
            --update_bench \
            --ssd_lat_off \
            --seed "${SEED}" \
            --keylen_mean "${KEYLEN}" \
            --variable_keylen \
            > "${LOG_FILE}" 2>&1

        AVG_IOPS=$(grep -oP 'update_iops=\K[\d.]+' "${LOG_FILE}" | head -1)
        echo "  Log: ${LOG_FILE}"
        echo "  Avg IOPS: ${AVG_IOPS:-N/A}"

        case "${KEYLEN}" in
            16) IOPS_16+=("${AVG_IOPS:-0}") ;;
            32) IOPS_32+=("${AVG_IOPS:-0}") ;;
            64) IOPS_64+=("${AVG_IOPS:-0}") ;;
        esac
    done

    # ----- 当前 map_frac 的 IOPS 写入 CSV -----
    # map_frac=0.2500 -> "map64p256"
    # map_frac=1.0000 -> "map256p256"
    SCHEME_NAME="map_frac=${MAP_FRAC}"

    # 格式化：取第一个值（各自只跑一次），对齐列宽
    printf "%-18s %14s,  %14s,  %14s\n" \
        "${SCHEME_NAME}" \
        "${IOPS_16[0]:-0}" \
        "${IOPS_32[0]:-0}" \
        "${IOPS_64[0]:-0}" >> "${RESULTS_CSV}"

    echo ""
    echo "  === map_frac=${MAP_FRAC} summary ==="
    printf "  %-12s  %14s  %14s  %14s\n" "keylen" "16B" "32B" "64B"
    printf "  %-12s  %14s  %14s  %14s\n" "IOPS" "${IOPS_16[0]:-0}" "${IOPS_32[0]:-0}" "${IOPS_64[0]:-0}"
done

echo ""
echo "============================================"
echo " Keylen Sensitivity done!"
echo "============================================"
echo ""
echo "Results: ${RESULTS_CSV}"
echo ""
echo "=== Results ==="
cat "${RESULTS_CSV}"