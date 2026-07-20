#!/bin/bash
# YCSB Workload A-F 测试脚本
# 用法: bash scripts/run_ycsb.sh [--ycsb_ops N] [--pool_size N] [--keylen_mean N]
#
# 配置: 128GB pool, 可变长键
# 独立进程：每个 workload 单独 load → 测试 → exit → 记录结果
# 结果保存到 ycsb_results_<timestamp>/

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

RESULTS_DIR="ycsb_results_$(date +%Y%m%d_%H%M%S)"
SUMMARY_FILE="$RESULTS_DIR/summary.txt"
WORKLOADS=(a b c d e f)

# 默认参数
POOL_SIZE=134217728        # 128GB (128 * 1048576)
YCSB_OPS=2000000
KEYLEN_MEAN=32               # 32B 中键

# 解析命令行参数覆盖默认值
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ycsb_ops)
            YCSB_OPS="$2"; shift 2 ;;
        --pool_size)
            POOL_SIZE="$2"; shift 2 ;;
        --keylen_mean)
            KEYLEN_MEAN="$2"; shift 2 ;;
        *)
            echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS_DIR"

echo "============================================"
echo " YCSB Workload A-F Benchmark"
echo "============================================"
echo " Project:    hash_kvftl"
echo " Workloads:  ${WORKLOADS[*]}"
echo " Pool:       ${POOL_SIZE} items"
echo " Ops/work:   ${YCSB_OPS}"
echo " KeyLen:     ${KEYLEN_MEAN}B (variable)"
echo " Results:    ${RESULTS_DIR}/"
echo "============================================"
echo ""

# ----- 编译 -----
echo "[$(date '+%H:%M:%S')] make clean..."
make clean 2>&1 | tail -1
echo "[$(date '+%H:%M:%S')] make test_ext_mem_lat..."
make test_ext_mem_lat 2>&1 | tail -3
BINARY="hash_hot_cmt/test_ext_mem_lat"

# NUMA 亲和性：默认 node 0，可通过环境变量 NUMA_NODE 覆盖
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"; NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    exit 1
fi
echo ""

# ----- 摘要表头 -----
cat > "$SUMMARY_FILE" <<'EOF'
========================================================
 YCSB Benchmark Summary
========================================================
Workload   TotalOps    AvgIOPS    AvgLat(us)  MaxLat(us)  HitRate(%)
--------   --------    -------    ----------  ----------  ----------
EOF

# ----- 运行 YCSB Workload A-F -----
for workload in "${WORKLOADS[@]}"; do
    echo "============================================"
    echo " [$(date '+%H:%M:%S')] YCSB Workload $workload start"
    echo "============================================"

    LOG_FILE="$RESULTS_DIR/ycsb_${workload}.log"

    # 每个 workload 起独立进程：load → 测试 → exit
    if ! eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" ./hash_hot_cmt/test_ext_mem_lat \
        --pool_size "$POOL_SIZE" \
        --ycsb "$workload" \
        --ycsb_ops "$YCSB_OPS" \
        --keylen_mean "$KEYLEN_MEAN" \
        --variable_keylen > "$LOG_FILE" 2>&1; then
        echo "ERROR: Workload $workload failed (see $LOG_FILE)"
        printf "   %c       %8s    %9s    %10s    %10s    %s\n" \
            "$workload" "FAILED" "-" "-" "-" "-" >> "$SUMMARY_FILE"
        continue
    fi

    # 提取关键指标
    TOTAL_OPS=$(grep -oP 'Total ops:\s*\K[\d.]+' "$LOG_FILE" | head -1)
    AVG_IOPS=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "$LOG_FILE" | head -1)
    AVG_LAT=$(grep -oP 'Avg latency:\s*\K[\d.]+' "$LOG_FILE" | head -1)
    MAX_LAT=$(grep -oP 'Max latency:\s*\K[\d.]+' "$LOG_FILE" | head -1)
    HIT_RATE=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "$LOG_FILE" | head -1)

    printf "   %c       %8s    %9s    %10s    %10s    %s%%\n" \
        "$workload" \
        "${TOTAL_OPS:-N/A}" \
        "${AVG_IOPS:-N/A}" \
        "${AVG_LAT:-N/A}" \
        "${MAX_LAT:-N/A}" \
        "${HIT_RATE:-N/A}" >> "$SUMMARY_FILE"

    echo ""
    echo "  Workload $workload done."
    echo "  Log:       $LOG_FILE"
    echo "  IOPS:      ${AVG_IOPS:-N/A}"
    echo "  Avg Lat:   ${AVG_LAT:-N/A} us"
    echo "  Max Lat:   ${MAX_LAT:-N/A} us"
    echo "  Hit Rate:  ${HIT_RATE:-N/A}%"
    echo ""
done

# ----- 结束 -----
cat >> "$SUMMARY_FILE" <<EOF
--------------------------------------------------------
 Finished at: $(date)
--------------------------------------------------------
EOF

echo "============================================"
echo " YCSB Workload A-F complete!"
echo "============================================"
echo ""
echo "Results directory: $RESULTS_DIR/"
echo "  - Full logs:   ycsb_{a,b,c,d,e,f}.log"
echo "  - Summary:     summary.txt"
echo ""
echo "=== Summary ==="
cat "$SUMMARY_FILE"
