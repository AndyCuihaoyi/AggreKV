#!/bin/bash
# YCSB Workload A perf profiling 脚本
# 用途：启动 YCSB A binary，您手动 attach perf record
#
# 使用方式：
#   1. bash scripts/run_ycsb_perf_profile.sh
#   2. 脚本启动 binary 后输出 PID 和 perf attach 命令
#   3. 您手动执行 perf 命令
#   4. 等待执行完成，查看完整日志
#
# 输出：
#   - perf_profile_<date>/ycsb_a.log

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

RESULTS_DIR="perf_profile_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

# 配置：pool 64GB，map_size_frac=0.25，YCSB A 操作数
POOL_SIZE=67108864     # 64GB
MAP_SIZE_FRAC=0.2500   # 64/256
YCSB_OPS=100000000    # 100M ops（可调整）

PERF_DATA="${RESULTS_DIR}/perf.data"

echo "============================================"
echo " YCSB Workload A — Perf Profile"
echo "============================================"
echo " Pool:       ${POOL_SIZE} items (64GB)"
echo " MapFrac:    ${MAP_SIZE_FRAC} (64/256)"
echo " YCSB ops:  ${YCSB_OPS}"
echo " Results:   ${RESULTS_DIR}/"
echo "============================================"
echo ""

# ---------- 编译（带 -g 用于火焰图） ----------
echo "[$(date '+%H:%M:%S')] make clean..."
make clean 2>&1 | tail -1
echo "[$(date '+%H:%M:%S')] make test_ext_mem_lat (with -g)..."
make test_ext_mem_lat 2>&1 | tail -3
BINARY="hash_hot_cmt/test_ext_mem_lat"
if [ ! -f "${BINARY}" ]; then
    echo "ERROR: Binary not found at ${BINARY}"
    exit 1
fi
echo ""

# ---------- 启动 binary ----------
echo "[$(date '+%H:%M:%S')] Starting binary in background..."
echo ""

LOG_FILE="${RESULTS_DIR}/ycsb_a.log"
${BINARY} \
    --pool_size "${POOL_SIZE}" \
    --map_size_frac "${MAP_SIZE_FRAC}" \
    --ycsb a \
    --ycsb_ops "${YCSB_OPS}" \
    > "${LOG_FILE}" 2>&1 &
BINARY_PID=$!

# 等待 binary 启动
sleep 3

# 检查进程是否还在运行
if ! kill -0 "${BINARY_PID}" 2>/dev/null; then
    echo "ERROR: Binary exited early. Check log: ${LOG_FILE}"
    cat "${LOG_FILE}"
    exit 1
fi

echo "============================================"
echo " Binary is running (PID=${BINARY_PID})"
echo " Log file:   ${LOG_FILE}"
echo ""
echo " Monitor progress:"
echo "   tail -f ${LOG_FILE}"
echo ""
echo "============================================"
echo " When YCSB A starts, run this command to attach perf:"
echo ""
echo "====== perf attach command ======"
echo "perf record -g -p ${BINARY_PID} -o ${PERF_DATA} -- sleep 30"
echo "====== perf attach command ======"
echo "============================================"
echo ""

# 等待 binary 结束
wait "${BINARY_PID}" || true
EXIT_CODE=$?

echo ""
echo "[$(date '+%H:%M:%S')] Binary exited (code=${EXIT_CODE})"
echo ""

# ---------- 打印完整日志 ----------
echo "============================================"
echo " Full log contents:"
echo "============================================"
cat "${LOG_FILE}"

echo ""
echo "============================================"
echo " Done. Log: ${LOG_FILE}"
echo "============================================"