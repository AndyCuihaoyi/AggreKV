#!/bin/bash
# ============================================================================
#  Hash-KVSSD (AggreKV) All-in-One Test Suite
# ============================================================================
#
#  串行运行以下 8 大实验，并按 [EXP_TAG] / [SUMMARY] / [END_STATS] 等标记
#  行从日志中解析关键指标，写入汇总表：
#
#    E1: Read 吞吐    (3 pool × {uniform, zipfian})
#    E2: Update 吞吐  (3 pool × {uniform, zipfian})
#    E3: Tail 延迟    (3 pool × zipfian, iodepth=1, p95/99/99.9/99.99)
#    E4: YCSB A keylen 敏感度 (16B / 32B / 64B)
#    E5: Update keylen 敏感度 (16B / 32B / 64B)
#    E6: YCSB A-F @ 128GB
#    E7: YCSB A-F @ 256GB
#    E8: GC wear       (10GB pool, zipfian update, ssd_lat_off)
#    E9: Perf profile  (YCSB A, 需手动 perf attach; 由 SKIP_PERF=1 跳过)
#
#  每个实验块开头都强制 clean+make（PROFILE_X_CFLAGS），宏变化时二进制必然更新。
#
#  用法：
#    bash run_all_hashkvssd_tests.sh              # 默认数据量（生产）
#    bash run_all_hashkvssd_tests.sh --smoke      # 小数据量冒烟（验证脚本）
#    bash run_all_hashkvssd_tests.sh --only E1    # 仅跑 E1
#    NUMA_NODE=1 bash run_all_hashkvssd_tests.sh  # NUMA 绑定
#    RUN_NOW=1 bash run_all_hashkvssd_tests.sh    # 立即执行（默认仅 dry-run 校验）
#
#  全部结果落在  all_test_results_<timestamp>/  下：
#      logs/                 每实验一个 .log（按 [EXP_TAG] 分组）
#      summary/summary.txt   汇总表（人读）
#      summary/summary.csv   汇总表（CSV，可导入 pandas）
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ============================================================================
# 默认参数（生产模式）
# ============================================================================
NR_G=1048575  # 1GB items

# Pool 配置（GB, map_frac）；本次全量使用 64/128/256 GB（用户指定）
POOLS_G=(64 128 256)
POOL_FRACS=(0.2500 0.5000 1.0000)

# Keylen 敏感度
KEYLENS=(16 32 64)

# YCSB 操作数（生产；原 2M/80M/100M 缩放到 1/8，加速测试；文件名/tag 不变）
YCSB_OPS=250000                  # 原 2,000,000  → 1/8
YCSB_OPS_KEYLEN=10000000         # 原 80,000,000 → 1/8
YCSB_OPS_PERF=12500000           # 原 100,000,000 → 1/8

# E6/E7 full YCSB-A-F sweep (separate from E1-E5 Pools).
#   E6: 128 GB pool, map_frac 0.5  (half of pool is mapping table → ~150 GiB live)
#   E7: 256 GB pool, map_frac 1.0  (full pool mapped; needs >= ~280 GiB RAM)
E6_POOL_GB=128
E6_MAP_FRAC=0.5000
E7_POOL_GB=256
E7_MAP_FRAC=1.0000
E6_E7_YCSB_OPS=4000000
# YCSB 'e' (range scans) on full-size pools emits very large read sets that
# overflow the binary's tracking range and produce N/A in the summary.
# For E6/E7 only, run YCSB 'e' at 1/100 ops so it stays in range. The log
# filename keeps the original tag (E6 / E7) so downstream parsing is unchanged.
E6_E7_YCSB_OPS_E=$((E6_E7_YCSB_OPS / 100))

# Update / Read 操作数（原 16M/64M/10M 缩放到 1/8；文件名/tag 不变）
UPDATE_NUM_POOL=$((2 * NR_G))    # 原 16M → 2,097,152 (≈2M)
UPDATE_NUM_KEYLEN=$((8 * NR_G))  # 原 64M → 8,388,608  (≈8M)
UPDATE_NUM_GC=$((1310720))       # 原 10M → 1,310,720  (≈1.25M)
READ_NUM_FACTOR=1                 # read_num = pool_size * factor / NUM_WORKERS（系数，与缩放无关）

# GC wear pool
GC_WEAR_POOL_GB=10
GC_WEAR_POOL=$((GC_WEAR_POOL_GB * NR_G))

# 编译宏（每个实验一个 profile）
COMMON_CFLAGS_BASE="-g -Wall -fcommon -lm -lglib-2.0"
PROFILE_E1_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DADAPTIVE_MEM"
PROFILE_E2_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DADAPTIVE_MEM"
PROFILE_E3_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DADAPTIVE_MEM"
PROFILE_E4_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DADAPTIVE_MEM"
PROFILE_E5_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DADAPTIVE_MEM"
PROFILE_E6_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DDATA_SEGREGATION -DADAPTIVE_MEM"
PROFILE_E7_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DDATA_SEGREGATION -DADAPTIVE_MEM"
PROFILE_E8_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DDATA_SEGREGATION -DADAPTIVE_MEM"
PROFILE_E9_CFLAGS="${COMMON_CFLAGS_BASE} -DHOT_CMT -DDATA_SEGREGATION -DADAPTIVE_MEM"

# ============================================================================
# 冒烟测试参数（SMOKE_MODE）
# ============================================================================
SMOKE_MODE=0
SMOKE_POOL_SIZE=$((64 * NR_G / 1024))   # 64MB items (≈ pool_size = 64K)
SMOKE_MAP_FRAC=0.2500
SMOKE_YCSB_OPS=5000
SMOKE_UPDATE_NUM=5000
SMOKE_READ_NUM=5000
SMOKE_KEYLEN=16
SMOKE_GC_POOL_GB=1
SMOKE_GC_POOL=$((SMOKE_GC_POOL_GB * NR_G / 1024 * 1024))  # 占位

# ============================================================================
# CLI 解析
# ============================================================================
ONLY_EXPS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --smoke)
            SMOKE_MODE=1
            shift
            ;;
        --only)
            ONLY_EXPS+=("$2")
            shift 2
            ;;
        --num)
            NR_G="$2"; shift 2
            ;;
        --ycsb_ops)
            YCSB_OPS="$2"; shift 2
            ;;
        --help|-h)
            grep -E '^#  ' "$0" | head -40
            exit 0
            ;;
        *)
            echo "Unknown option: $1"; exit 1
            ;;
    esac
done

# 冒烟模式：把所有数据量压到最小
if [[ "${SMOKE_MODE}" == "1" ]]; then
    echo "[SMOKE MODE] 启用冒烟测试（小数据量）"
    POOLS_G=(1)               # 1GB → 实测极小值
    POOL_FRACS=(0.2500)
    KEYLENS=(16 32)
    YCSB_OPS=${SMOKE_YCSB_OPS}                # 5000
    YCSB_OPS_KEYLEN=${SMOKE_YCSB_OPS}         # 冒烟下也压到 5000
    YCSB_OPS_PERF=${SMOKE_YCSB_OPS}
    UPDATE_NUM_POOL=${SMOKE_UPDATE_NUM}        # 5000
    UPDATE_NUM_KEYLEN=${SMOKE_UPDATE_NUM}      # 冒烟下也压到 5000
    UPDATE_NUM_GC=${SMOKE_UPDATE_NUM}
    GC_WEAR_POOL_GB=1
    GC_WEAR_POOL=$((1 * NR_G / 1024))         # 1MB items
fi

# ============================================================================
# 路径与目录（固定 hash_kvssd_results/，不带日期后缀）
# ============================================================================
RESULTS_DIR="hash_kvssd_results"
LOG_DIR="${RESULTS_DIR}/logs"
SUMMARY_DIR="${RESULTS_DIR}/summary"
# 启动时清空旧结果，保持干净
rm -rf "${RESULTS_DIR}"
mkdir -p "${LOG_DIR}" "${SUMMARY_DIR}"

BIN="hash_hot_cmt/test_ext_mem_lat"

# NUMA
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"
NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

# ============================================================================
# 通用函数
# ============================================================================

# build_aggrekv <profile_cflags>
#   直接调用 gcc 编译，避免命令行 CFLAGS 覆盖 module.mk 中目标专属的 += 规则。
#   必须在 clean 后从源码逐个 .c 编 .o，再链接。
build_aggrekv() {
    local extra="$1"
    echo ""
    echo "[$(date '+%H:%M:%S')] ===== clean + compile all ====="
    echo "  EXTRA_CFLAGS: ${extra}"
    make clean 2>&1 | tail -1

    local COMMON="-g -Wall -fcommon -fno-omit-frame-pointer"
    local GLIB_INC="-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/"
    local CFLAGS="${COMMON} ${extra} ${GLIB_INC}"

    local SRC_FILES=$(find tools lower hash_hot_cmt -name '*.c')
    local OBJ_FILES=""
    local FAIL=0
    for src in ${SRC_FILES}; do
        local obj="${src%.c}.o"
        OBJ_FILES="${OBJ_FILES} ${obj}"
        if ! cc -MMD -MP -c -o "${obj}" "${src}" ${CFLAGS} 2>&1; then
            echo "[ERROR] compile ${src} failed"
            FAIL=1
            break
        fi
    done
    if [[ "${FAIL}" == "1" ]]; then
        echo "[ERROR] build failed"
        return 1
    fi

    # 链接
    if ! cc ${OBJ_FILES} ${COMMON} -lm -lglib-2.0 -o "${BIN}" 2>&1; then
        echo "[ERROR] link failed"
        return 1
    fi

    if [ ! -f "${BIN}" ]; then
        echo "[ERROR] Binary not found at ${BIN}"
        return 1
    fi
    echo "  ✓ Binary built: ${BIN}"
}

# run_one <exp_id> <exp_tag> <log_name> <binary_args...>
#   包装 numactl + log 重定向；失败不终止（继续后续实验）
run_one() {
    local exp_id="$1"; shift
    local exp_tag="$1"; shift
    local log_name="$1"; shift
    local log_file="${LOG_DIR}/${log_name}.log"

    echo "  [E${exp_id}] log: ${log_file}"
    eval "${NUMACTL_CMD}" "${NUMACTL_ARGS}" "${BIN}" \
        --experiment_tag "${exp_tag}" \
        "$@" > "${log_file}" 2>&1 || {
        echo "  [WARN] E${exp_id} ${exp_tag} failed (see ${log_file})"
        return 0
    }
}

# ----- 过滤函数：仅当 --only 指定时才运行 -----
should_run() {
    local eid="$1"
    if [[ ${#ONLY_EXPS[@]} -eq 0 ]]; then return 0; fi
    for x in "${ONLY_EXPS[@]}"; do
        [[ "$x" == "$eid" ]] && return 0
    done
    return 1
}

# ============================================================================
# 实验 1: Read 吞吐 (3 pool × 2 dist)
# ============================================================================
exp1_read_throughput() {
    echo ""
    echo "######################################################################"
    echo "# E1: Read Throughput (3 pool × {uniform, zipfian})"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E1_CFLAGS}" || return 1
    local idx=0
    for pg in "${POOLS_G[@]}"; do
        local frac="${POOL_FRACS[$idx]}"
        local pool=$((pg * NR_G))
        local num_read=$((pool * READ_NUM_FACTOR / 7))
        for dist in uniform zipfian; do
            run_one "1" "read_pool${pg}G_${dist}" \
                "E1_read_pool${pg}G_${dist}" \
                --pool_size "${pool}" \
                --map_size_frac "${frac}" \
                --num_read "${num_read}" \
                --read_dist "${dist}" \
                --read_bench \
                --seed 1
        done
        idx=$((idx + 1))
    done
}

# ============================================================================
# 实验 2: Update 吞吐 (3 pool × 2 dist)
# ============================================================================
exp2_update_throughput() {
    echo ""
    echo "######################################################################"
    echo "# E2: Update Throughput (3 pool × {uniform, zipfian})"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E2_CFLAGS}" || return 1
    local idx=0
    for pg in "${POOLS_G[@]}"; do
        local frac="${POOL_FRACS[$idx]}"
        local pool=$((pg * NR_G))
        for dist in uniform zipfian; do
            run_one "2" "update_pool${pg}G_${dist}" \
                "E2_update_pool${pg}G_${dist}" \
                --pool_size "${pool}" \
                --map_size_frac "${frac}" \
                --num_update "${UPDATE_NUM_POOL}" \
                --update_dist "${dist}" \
                --update_bench \
                --seed 1
        done
        idx=$((idx + 1))
    done
}

# ============================================================================
# 实验 3: Tail 延迟 (3 pool, zipfian, iodepth=1)
# ============================================================================
exp3_tail_latency() {
    echo ""
    echo "######################################################################"
    echo "# E3: Tail Latency (3 pool, zipfian, iodepth=1)"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E3_CFLAGS}" || return 1
    local idx=0
    for pg in "${POOLS_G[@]}"; do
        local frac="${POOL_FRACS[$idx]}"
        local pool=$((pg * NR_G))
        local num_read=$((pool * READ_NUM_FACTOR / 7))
        run_one "3" "tail_pool${pg}G_zipf" \
            "E3_tail_pool${pg}G_zipf" \
            --pool_size "${pool}" \
            --map_size_frac "${frac}" \
            --num_read "${num_read}" \
            --read_dist zipfian \
            --read_bench \
            --seed 1
        idx=$((idx + 1))
    done
}

# ============================================================================
# 实验 4: YCSB A keylen 敏感度
# ============================================================================
exp4_ycsb_keylen() {
    echo ""
    echo "######################################################################"
    echo "# E4: YCSB A — Key-Length Sensitivity (${KEYLENS[*]} B)"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E4_CFLAGS}" || return 1
    # pool: 冒烟用 1GB（≈1M），生产用 64GB；map_frac=0.25
    local pool_gb=64
    if [[ "${SMOKE_MODE}" == "1" ]]; then pool_gb=1; fi
    local pool=$((pool_gb * NR_G))
    local frac=0.2500
    for kl in "${KEYLENS[@]}"; do
        run_one "4" "ycsb_a_keylen${kl}" \
            "E4_ycsb_a_keylen${kl}" \
            --pool_size "${pool}" \
            --map_size_frac "${frac}" \
            --ycsb a \
            --ycsb_ops "${YCSB_OPS_KEYLEN}" \
            --keylen_mean "${kl}" \
            --variable_keylen
    done
}

# ============================================================================
# 实验 5: Update keylen 敏感度
# ============================================================================
exp5_update_keylen() {
    echo ""
    echo "######################################################################"
    echo "# E5: Update — Key-Length Sensitivity (${KEYLENS[*]} B)"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E5_CFLAGS}" || return 1
    local pool_gb=64
    if [[ "${SMOKE_MODE}" == "1" ]]; then pool_gb=1; fi
    local pool=$((pool_gb * NR_G))
    local frac=0.2500
    for kl in "${KEYLENS[@]}"; do
        run_one "5" "update_keylen${kl}" \
            "E5_update_keylen${kl}" \
            --pool_size "${pool}" \
            --map_size_frac "${frac}" \
            --num_update "${UPDATE_NUM_KEYLEN}" \
            --update_dist zipfian \
            --update_bench \
            --ssd_lat_off \
            --keylen_mean "${kl}" \
            --variable_keylen
    done
}

# ============================================================================
# 实验 6/7: YCSB A-F
# ============================================================================
exp6_ycsb_full() {
    local exp_id="$1"; local pool_gb="$2"; local profile_cflags="$3"
    local ycsb_tag="$4"; local map_frac="$5"
    echo ""
    echo "######################################################################"
    echo "# E${exp_id}: YCSB A-F @ pool=${pool_gb}GB, map_frac=${map_frac}, ops=${E6_E7_YCSB_OPS}"
    echo "######################################################################"
    build_aggrekv "${profile_cflags}" || return 1
    local pool=$((pool_gb * NR_G))
    for w in a b c d e f; do
        # YCSB 'e' uses a smaller op count (see E6_E7_YCSB_OPS_E) to keep
        # its range-scan footprint within the binary's tracking range.
        local w_ops="${E6_E7_YCSB_OPS}"
        if [[ "${w}" == "e" ]]; then
            w_ops="${E6_E7_YCSB_OPS_E}"
        fi
        run_one "${exp_id}" "ycsb_${w}_pool${pool_gb}G_${ycsb_tag}" \
            "E${exp_id}_ycsb_${w}_pool${pool_gb}G_${ycsb_tag}" \
            --pool_size "${pool}" \
            --ycsb "${w}" \
            --ycsb_ops "${w_ops}" \
            --map_size_frac "${map_frac}" \
            --keylen_mean 32 \
            --variable_keylen
    done
}

# ============================================================================
# 实验 8: GC wear (10GB pool, zipfian update)
# ============================================================================
exp8_gc_wear() {
    echo ""
    echo "######################################################################"
    echo "# E8: GC Wear (pool=${GC_WEAR_POOL_GB}GB, zipfian update)"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E8_CFLAGS}" || return 1
    run_one "8" "gc_wear" "E8_gc_wear" \
        --pool_size "${GC_WEAR_POOL}" \
        --map_size_frac 1.0000 \
        --num_update "${UPDATE_NUM_GC}" \
        --update_dist zipfian \
        --update_bench \
        --ssd_lat_off \
        --seed 1
}

# ============================================================================
# 实验 9: Perf profile (手动 attach)
# ============================================================================
exp9_perf_profile() {
    if [[ "${SKIP_PERF:-0}" == "1" ]]; then
        echo "[E9] SKIP_PERF=1, 跳过 perf profile"
        return 0
    fi
    echo ""
    echo "######################################################################"
    echo "# E9: Perf Profile (YCSB A, 手动 perf attach)"
    echo "######################################################################"
    build_aggrekv "${PROFILE_E9_CFLAGS}" || return 1
    local pool=$((64 * NR_G))
    local log_file="${LOG_DIR}/E9_perf_ycsb_a.log"
    local perf_data="${LOG_DIR}/perf.data"
    "${BIN}" --experiment_tag "perf_ycsb_a" \
        --pool_size "${pool}" \
        --map_size_frac 0.2500 \
        --ycsb a \
        --ycsb_ops "${YCSB_OPS_PERF}" \
        > "${log_file}" 2>&1 &
    local pid=$!
    sleep 3
    if ! kill -0 "${pid}" 2>/dev/null; then
        echo "[E9] Binary exited early, see ${log_file}"
        return 0
    fi
    echo ""
    echo "==== Attach perf with: ===="
    echo "perf record -g -p ${pid} -o ${perf_data} -- sleep 30"
    echo "============================="
    wait "${pid}" || true
}

# ============================================================================
# 汇总：解析日志 → summary.txt + summary.csv
# ============================================================================
summarize_all() {
    local txt="${SUMMARY_DIR}/summary.txt"
    local csv="${SUMMARY_DIR}/summary.csv"
    : > "${txt}"
    : > "${csv}"

    cat > "${txt}" <<EOF
========================================================================
 Hash-KVSSD (AggreKV) Test Suite — Summary
========================================================================
Run started:  $(date '+%Y-%m-%d %H:%M:%S %Z')
NUMA node:    ${NUMA_NODE}
SMOKE mode:   ${SMOKE_MODE}
Results dir:  ${RESULTS_DIR}
========================================================================
EOF

    cat > "${csv}" <<EOF
exp,tag,total_ops,avg_iops,avg_lat_us,max_lat_us,hit_rate_pct,p95_us,p99_us,p99_9_us,p99_99_us,status
EOF

    # ----- E1: read bench (read_iops= / hit_rt=) -----
    cat >> "${txt}" <<EOF

[E1] Read Throughput
  Tag                              AvgIOPS   HitRate(%)
EOF
    for f in "${LOG_DIR}"/E1_read_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local iops=$(grep -oP 'read_iops=\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'hit_rt=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-9s  %s\n" "${tag}" "${iops:-N/A}" "${hit:-N/A}" >> "${txt}"
        echo "E1,${tag},N/A,${iops:-N/A},N/A,N/A,${hit:-N/A},N/A,N/A,N/A,N/A,$(if [[ -n "${iops:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
    done

    # ----- E2: update bench (update_iops=) -----
    cat >> "${txt}" <<EOF

[E2] Update Throughput
  Tag                              AvgIOPS
EOF
    for f in "${LOG_DIR}"/E2_update_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local iops=$(grep -oP 'update_iops=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %s\n" "${tag}" "${iops:-N/A}" >> "${txt}"
        echo "E2,${tag},N/A,${iops:-N/A},N/A,N/A,N/A,N/A,N/A,N/A,N/A,$(if [[ -n "${iops:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
    done

    # ----- E3: tail latency (avg_rlat/us: | p95_rlat/us: | ...) -----
    cat >> "${txt}" <<EOF

[E3] Tail Latency (iodepth=1, zipfian)
  Tag                              Avg(us)   p95(us)   p99(us)   p99.9(us)  p99.99(us)
EOF
    for f in "${LOG_DIR}"/E3_tail_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local line=$(grep -m1 "avg_rlat/us:" "$f" || true)
        if [[ -n "${line}" ]]; then
            local vals=$(echo "${line}" | awk -F'|' '{
                for (i=2; i<=6; i++) gsub(/[^0-9]/, "", $i);
                printf "%s %s %s %s %s\n", $2, $3, $4, $5, $6;
            }')
            printf "  %-32s  %s\n" "${tag}" "${vals}" >> "${txt}"
            echo "E3,${tag},N/A,N/A,$(echo $vals | awk '{print $1}'),N/A,N/A,$(echo $vals | awk '{print $2}'),$(echo $vals | awk '{print $3}'),$(echo $vals | awk '{print $4}'),$(echo $vals | awk '{print $5}'),OK" >> "${csv}"
        else
            printf "  %-32s  N/A (no tail-latency line found)\n" "${tag}" >> "${txt}"
            echo "E3,${tag},N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,FAIL" >> "${csv}"
        fi
    done

    # ----- E4: YCSB keylen sensitivity -----
    cat >> "${txt}" <<EOF

[E4] YCSB Key-Length Sensitivity
  Tag                              KeyLen   TotalOps   AvgIOPS   AvgLat(us)  HitRate(%)
EOF
    for f in "${LOG_DIR}"/E4_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local kl=$(echo "${tag}" | grep -oP 'keylen\K[0-9]+')
        local ops=$(grep -oP 'Total ops:\s*\K[\d.]+' "$f" | head -1)
        local iops=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "$f" | head -1)
        local lat=$(grep -oP 'Avg latency:\s*\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-7s  %-9s  %-8s  %-10s  %s\n" \
            "${tag}" "${kl:-N/A}" "${ops:-N/A}" "${iops:-N/A}" "${lat:-N/A}" "${hit:-N/A}" >> "${txt}"
        echo "E4,${tag},${ops:-N/A},${iops:-N/A},${lat:-N/A},N/A,${hit:-N/A},N/A,N/A,N/A,N/A,$(if [[ -n "${iops:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
    done

    # ----- E5: Update bench keylen sensitivity -----
    cat >> "${txt}" <<EOF

[E5] Update Key-Length Sensitivity
  Tag                              KeyLen   UpdateNum   UpdateIOPS  HitRate(%)
EOF
    for f in "${LOG_DIR}"/E5_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local kl=$(echo "${tag}" | grep -oP 'keylen\K[0-9]+')
        # update_bench 的 SUMMARY 行格式：
        #   [SUMMARY] pool_size=..., update_dist=..., update_num=N, iodepth=..., update_iops=X.XX, hit_rt=Y.YYYY%
        local upd_num=$(grep -oP 'update_num=\K[0-9]+' "$f" | head -1)
        local upd_iops=$(grep -oP 'update_iops=\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'hit_rt=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-7s  %-9s  %-10s  %s\n" \
            "${tag}" "${kl:-N/A}" "${upd_num:-N/A}" "${upd_iops:-N/A}" "${hit:-N/A}" >> "${txt}"
        echo "E5,${tag},${upd_num:-N/A},${upd_iops:-N/A},N/A,N/A,${hit:-N/A},N/A,N/A,N/A,N/A,$(if [[ -n "${upd_iops:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
    done

    # ----- E6/E7: YCSB A-F -----
    for exp in 6 7; do
        cat >> "${txt}" <<EOF

[E${exp}] YCSB A-F
  Tag                              Workload  TotalOps   AvgIOPS   AvgLat(us)  MaxLat(us)  HitRate(%)
EOF
        for f in "${LOG_DIR}"/E${exp}_ycsb_*.log; do
            [[ ! -f "$f" ]] && continue
            local tag=$(basename "$f" .log)
            local wl=$(echo "${tag}" | grep -oP 'ycsb_\K[a-f]')
            local ops=$(grep -oP 'Total ops:\s*\K[\d.]+' "$f" | head -1)
            local iops=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "$f" | head -1)
            local lat=$(grep -oP 'Avg latency:\s*\K[\d.]+' "$f" | head -1)
            local max=$(grep -oP 'Max latency:\s*\K[\d.]+' "$f" | head -1)
            local hit=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "$f" | head -1)
            printf "  %-32s  %-8s  %-9s  %-8s  %-10s  %-10s  %s\n" \
                "${tag}" "${wl:-N/A}" "${ops:-N/A}" "${iops:-N/A}" "${lat:-N/A}" "${max:-N/A}" "${hit:-N/A}" >> "${txt}"
            echo "E${exp},${tag},${ops:-N/A},${iops:-N/A},${lat:-N/A},${max:-N/A},${hit:-N/A},N/A,N/A,N/A,N/A,$(if [[ -n "${iops:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
        done
    done

    # ----- E8: GC wear (total GC erase count / sblk[N]: gc_cnt=) -----
    cat >> "${txt}" <<EOF

[E8] GC Wear
  Tag                              TotalErased   SBLKsWithData
EOF
    for f in "${LOG_DIR}"/E8_*.log; do
        [[ ! -f "$f" ]] && continue
        local tag=$(basename "$f" .log)
        local total=$(grep "total GC erase count:" "$f" | sed 's/.*: //' | head -1)
        local sblk_n=$(grep -c "sblk\[" "$f" || true)
        printf "  %-32s  %-12s  %s\n" "${tag}" "${total:-N/A}" "${sblk_n:-0}" >> "${txt}"
        echo "E8,${tag},N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,$(if [[ -n "${total:-}" ]]; then echo OK; else echo FAIL; fi)" >> "${csv}"
    done

    # ----- E9: perf profile -----
    if [[ -f "${LOG_DIR}/E9_perf_ycsb_a.log" ]]; then
        cat >> "${txt}" <<EOF

[E9] Perf Profile (YCSB A)
  Log: ${LOG_DIR}/E9_perf_ycsb_a.log
  Perf data: ${LOG_DIR}/perf.data
EOF
    fi

    cat >> "${txt}" <<EOF

========================================================================
Finished at: $(date)
========================================================================
EOF

    echo ""
    echo "===================== summary.txt ====================="
    cat "${txt}"
    echo ""
    echo "===================== summary.csv ====================="
    cat "${csv}"
}

# ============================================================================
# 配置落盘
# ============================================================================
# ============================================================================
# Main
# ============================================================================
echo "================================================"
echo " Hash-KVSSD (AggreKV) All-in-One Test Suite"
echo "================================================"
echo "Results dir: ${RESULTS_DIR}"
echo "NUMA node:   ${NUMA_NODE}"
echo "SMOKE mode:  ${SMOKE_MODE}"
if [[ ${#ONLY_EXPS[@]} -gt 0 ]]; then
    echo "Only run:    ${ONLY_EXPS[*]}"
fi
echo "================================================"


should_run "E1" && exp1_read_throughput || echo "[skip E1]"
should_run "E2" && exp2_update_throughput || echo "[skip E2]"
should_run "E3" && exp3_tail_latency || echo "[skip E3]"
should_run "E4" && exp4_ycsb_keylen || echo "[skip E4]"
should_run "E5" && exp5_update_keylen || echo "[skip E5]"
should_run "E6" && exp6_ycsb_full 6 "${E6_POOL_GB}" "${PROFILE_E6_CFLAGS}" "E6" "${E6_MAP_FRAC}" || echo "[skip E6]"
should_run "E7" && exp6_ycsb_full 7 "${E7_POOL_GB}" "${PROFILE_E7_CFLAGS}" "E7" "${E7_MAP_FRAC}" || echo "[skip E7]"
should_run "E8" && exp8_gc_wear || echo "[skip E8]"
should_run "E9" && exp9_perf_profile || echo "[skip E9]"

echo ""
echo "================================================"
echo " Summarizing..."
echo "================================================"
summarize_all

echo ""
echo "All logs:     ${LOG_DIR}/"
echo "Summary txt:  ${SUMMARY_DIR}/summary.txt"
echo "Summary csv:  ${SUMMARY_DIR}/summary.csv"
