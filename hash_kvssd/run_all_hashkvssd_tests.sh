#!/bin/bash
# ============================================================================
#  Hash-KVSSD (AggreKV) All-in-One Test Suite
# ============================================================================
#
#  Hash-KVSSD (AggreKV) — All-in-One Test Suite
#
#  Runs nine experiments (E1..E9) and parses [EXP_TAG] / [SUMMARY] /
#  [END_STATS] markers from each log into a per-experiment row in
#  hash_kvssd_results/summary/.
#
#    E1: Read throughput   (3 pool × {uniform, zipfian})
#    E2: Update throughput (3 pool × {uniform, zipfian})
#    E3: Tail latency      (3 pool × zipfian, iodepth=1, p95/99/99.9/99.99)
#    E4: YCSB A — key-length sensitivity (16B / 32B / 64B)
#    E5: Update — key-length sensitivity (16B / 32B / 64B)
#    E6: YCSB A-F @ 128 GB pool, map_frac 0.5
#    E7: YCSB A-F @ 256 GB pool, map_frac 1.0
#    E8: GC wear           (10 GB pool, zipfian update, ssd_lat_off)
#    E9: Perf profile      (YCSB A, manual perf attach; skipped by default)
#
#  Usage:
#    bash run_all_hashkvssd_tests.sh                  # full sweep (all variants)
#    bash run_all_hashkvssd_tests.sh aggrekv          # AggreKV variant only
#    bash run_all_hashkvssd_tests.sh rhik             # RHIK variant only
#    bash run_all_hashkvssd_tests.sh --smoke          # small-data smoke test
#    bash run_all_hashkvssd_tests.sh --only E1        # run a single experiment
#    bash run_all_hashkvssd_tests.sh rhik --only E6   # combine variant + filter
#    NUMA_NODE=1 bash run_all_hashkvssd_tests.sh      # bind to NUMA node 1
#
#  Variants:
#    AggreKV — proposed design (HOT_CMT, ADAPTIVE_MEM, DATA_SEGREGATION)
#    RHIK    — baseline without the three AggreKV optimizations
#              (HOT_CMT / ADAPTIVE_MEM / DATA_SEGREGATION macros are unset)
#    all     — run both variants in sequence (default)
#
#  Each variant produces its own log files and summary section, distinguished
#  by the suffix _AggreKV or _RHIK on the log file name.
#
#  All results land in hash_kvssd_results/logs/:
#      <EXP_TAG>_<variant>.log   one log per (experiment, variant) cell,
#                                suffixed with _AggreKV or _RHIK
#      perf.data                 (E9 only) perf record output, kept as-is
#
#  No aggregate summary files are written. This artifact targets the
#  Functional badge (script-execution passes); per-experiment numbers
#  stay inside each log via [SUMMARY] markers and are greppable as
#  `grep '\[SUMMARY\]' logs/*.log`.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Default variant mode; overridable via positional argument below.
VARIANT_MODE="all"

# ============================================================================
# Production defaults (non-smoke)
# ============================================================================
NR_G=1048575  # 1GB items

# Pools (GB) paired with POOL_FRACS at the same index.
POOLS_G=(64 128 256)
POOL_FRACS=(0.2500 0.5000 1.0000)

# Key-length sensitivity values
KEYLENS=(16 32 64)

# YCSB ops per workload (see exp* functions for usage)
YCSB_OPS=250000                  # (production scale)
YCSB_OPS_KEYLEN=10000000         # (production scale)
YCSB_OPS_PERF=12500000           # (production scale)

# E6/E7 full YCSB-A-F sweep (separate from E1-E5 pools).
E6_POOL_GB=128
E6_MAP_FRAC=0.5000
E7_POOL_GB=256
E7_MAP_FRAC=1.0000
E6_E7_YCSB_OPS=4000000
E6_E7_YCSB_OPS_E=$((E6_E7_YCSB_OPS / 100))

# Update / Read op counts (see exp* functions for usage)
UPDATE_NUM_POOL=$((2 * NR_G))    # (= 2 * NR_G)
UPDATE_NUM_KEYLEN=$((8 * NR_G))  # (= 8 * NR_G)
UPDATE_NUM_GC=$((1310720))       # (fixed value)
READ_NUM_FACTOR=1                 # read_num = pool_size * factor / NUM_WORKERS

# GC wear pool size
GC_WEAR_POOL_GB=10
GC_WEAR_POOL=$((GC_WEAR_POOL_GB * NR_G))

# Per-experiment compile profiles
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


# RHIK baseline variant: same COMMON_CFLAGS_BASE but without the three
# AggreKV-specific optimization macros (-DHOT_CMT, -DADAPTIVE_MEM, -DDATA_SEGREGATION).
PROFILE_E1_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E2_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E3_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E4_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E5_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E6_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E7_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E8_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"
PROFILE_E9_CFLAGS_RHIK="${COMMON_CFLAGS_BASE}"

# ============================================================================
# Smoke-mode defaults
# ============================================================================
SMOKE_MODE=0
SMOKE_POOL_SIZE=$((64 * NR_G / 1024))   # 64MB items (≈ pool_size = 64K)
SMOKE_MAP_FRAC=0.2500
SMOKE_YCSB_OPS=5000
SMOKE_UPDATE_NUM=5000
SMOKE_READ_NUM=5000
SMOKE_KEYLEN=16
SMOKE_GC_POOL_GB=1
SMOKE_GC_POOL=$((SMOKE_GC_POOL_GB * NR_G / 1024 * 1024))  # placeholder

# ============================================================================
# CLI parsing
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
        aggrekv|rhik|all)
            VARIANT_MODE="$1"
            shift
            ;;
        *)
            echo "Unknown option: $1"; exit 1
            ;;
    esac
done

if [[ "${VARIANT_MODE}" != "aggrekv" && "${VARIANT_MODE}" != "rhik" && "${VARIANT_MODE}" != "all" ]]; then
    echo "Invalid variant mode: ${VARIANT_MODE} (expected: aggrekv|rhik|all)"; exit 1
fi

# Smoke-mode override (compact everything)
if [[ "${SMOKE_MODE}" == "1" ]]; then
    echo "[SMOKE MODE] enabled (compact data sizes)"
    POOLS_G=(1)               # 1 GB, smallest meaningful pool
    POOL_FRACS=(0.2500)
    KEYLENS=(16 32)
    YCSB_OPS=${SMOKE_YCSB_OPS}                # 5000
    YCSB_OPS_KEYLEN=${SMOKE_YCSB_OPS}         # (= SMOKE_YCSB_OPS / SMOKE_UPDATE_NUM)
    YCSB_OPS_PERF=${SMOKE_YCSB_OPS}
    UPDATE_NUM_POOL=${SMOKE_UPDATE_NUM}        # 5000
    UPDATE_NUM_KEYLEN=${SMOKE_UPDATE_NUM}      # (= SMOKE_YCSB_OPS / SMOKE_UPDATE_NUM)
    UPDATE_NUM_GC=${SMOKE_UPDATE_NUM}
    GC_WEAR_POOL_GB=1
    GC_WEAR_POOL=$((1 * NR_G / 1024))         # 1MB items
fi

# ============================================================================
# Paths and directories
# ============================================================================
RESULTS_DIR="hash_kvssd_results"
LOG_DIR="${RESULTS_DIR}/logs"
# No aggregate summary files are written (Functional badge only).
SUMMARY_DIR=""
# Clear previous results
rm -rf "${RESULTS_DIR}"
mkdir -p "${LOG_DIR}"

BIN="hash_hot_cmt/test_ext_mem_lat"

# NUMA
NUMA_NODE="${NUMA_NODE:-0}"
NUMACTL_CMD="numactl"
NUMACTL_ARGS="--cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"

# ============================================================================
# Common helpers
# ============================================================================

# build_aggrekv <profile_cflags>
#   Clean and rebuild the binary with the given CFLAGS profile.
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

    # Link step
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
#   Wraps numactl + log redirection; failures do not abort the run.
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

# ----- Variant selectors -----
# pick_variant_cflags <eid>
#   Return the CFLAGS profile for the current variant (AggreKV or RHIK).
pick_variant_cflags() {
    local eid="$1"
    if [[ "${VARIANT_MODE_FLAG:-AggreKV}" == "RHIK" ]]; then
        eval "echo "\${PROFILE_${eid}_CFLAGS_RHIK}""
    else
        eval "echo "\${PROFILE_${eid}_CFLAGS}""
    fi
}

# pick_variant_frac <pool_idx>
#   Return map_size_frac for the current variant. AggreKV uses POOL_FRACS;
#   RHIK always uses 1.0.
pick_variant_frac() {
    local idx="$1"
    if [[ "${VARIANT_MODE_FLAG:-AggreKV}" == "RHIK" ]]; then
        echo "1.0000"
    else
        echo "${POOL_FRACS[$idx]}"
    fi
}

# pick_variant_frac_fixed <aggrekv_frac>
pick_variant_frac_fixed() {
    local aggrekv_frac="$1"
    if [[ "${VARIANT_MODE_FLAG:-AggreKV}" == "RHIK" ]]; then
        echo "1.0000"
    else
        echo "${aggrekv_frac}"
    fi
}

# pick_variant_suffix
#   Return the log-file suffix for the current variant.
pick_variant_suffix() {
    echo "${VARIANT_MODE_FLAG:-AggreKV}"   # "AggreKV" or "RHIK"
}

# ----- Filter helper: only run if --only requested -----
should_run() {
    local eid="$1"
    if [[ ${#ONLY_EXPS[@]} -eq 0 ]]; then return 0; fi
    for x in "${ONLY_EXPS[@]}"; do
        [[ "$x" == "$eid" ]] && return 0
    done
    return 1
}

# ============================================================================
# Experiment 1: Read throughput (3 pool × 2 dist)
# ============================================================================
exp1_read_throughput() {
    echo ""
    echo "######################################################################"
    echo "# E1: Read Throughput (3 pool × {uniform, zipfian})  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E1)" || return 1
    local idx=0
    local suffix
    suffix="$(pick_variant_suffix)"
    for pg in "${POOLS_G[@]}"; do
        local frac
        frac="$(pick_variant_frac $idx)"
        local pool=$((pg * NR_G))
        local num_read=$((pool * READ_NUM_FACTOR / 7))
        for dist in uniform zipfian; do
            run_one "1" "read_pool${pg}G_${dist}" \
                "E1_read_pool${pg}G_${dist}_${suffix}" \
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
# Experiment 2: Update throughput (3 pool × 2 dist)
# ============================================================================
exp2_update_throughput() {
    echo ""
    echo "######################################################################"
    echo "# E2: Update Throughput (3 pool × {uniform, zipfian})  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E2)" || return 1
    local idx=0
    local suffix
    suffix="$(pick_variant_suffix)"
    for pg in "${POOLS_G[@]}"; do
        local frac
        frac="$(pick_variant_frac $idx)"
        local pool=$((pg * NR_G))
        for dist in uniform zipfian; do
            run_one "2" "update_pool${pg}G_${dist}" \
                "E2_update_pool${pg}G_${dist}_${suffix}" \
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
# Experiment 3: Tail latency (3 pool, zipfian, iodepth=1)
# ============================================================================
exp3_tail_latency() {
    echo ""
    echo "######################################################################"
    echo "# E3: Tail Latency (3 pool, zipfian, iodepth=1)  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E3)" || return 1
    local idx=0
    local suffix
    suffix="$(pick_variant_suffix)"
    for pg in "${POOLS_G[@]}"; do
        local frac
        frac="$(pick_variant_frac $idx)"
        local pool=$((pg * NR_G))
        local num_read=$((pool * READ_NUM_FACTOR / 7))
        run_one "3" "tail_pool${pg}G_zipf" \
            "E3_tail_pool${pg}G_zipf_${suffix}" \
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
# Experiment 4: YCSB A — key-length sensitivity
# ============================================================================
exp4_ycsb_keylen() {
    echo ""
    echo "######################################################################"
    echo "# E4: YCSB A — Key-Length Sensitivity (${KEYLENS[*]} B)  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E4)" || return 1
    local pool_gb=64
    if [[ "${SMOKE_MODE}" == "1" ]]; then pool_gb=1; fi
    local pool=$((pool_gb * NR_G))
    local frac
    frac="$(pick_variant_frac_fixed 0.2500)"
    local suffix
    suffix="$(pick_variant_suffix)"
    for kl in "${KEYLENS[@]}"; do
        run_one "4" "ycsb_a_keylen${kl}" \
            "E4_ycsb_a_keylen${kl}_${suffix}" \
            --pool_size "${pool}" \
            --map_size_frac "${frac}" \
            --ycsb a \
            --ycsb_ops "${YCSB_OPS_KEYLEN}" \
            --keylen_mean "${kl}" \
            --variable_keylen
    done
}

# ============================================================================
# Experiment 5: Update — key-length sensitivity
# ============================================================================
exp5_update_keylen() {
    echo ""
    echo "######################################################################"
    echo "# E5: Update — Key-Length Sensitivity (${KEYLENS[*]} B)  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E5)" || return 1
    local pool_gb=64
    if [[ "${SMOKE_MODE}" == "1" ]]; then pool_gb=1; fi
    local pool=$((pool_gb * NR_G))
    local frac
    frac="$(pick_variant_frac_fixed 0.2500)"
    local suffix
    suffix="$(pick_variant_suffix)"
    for kl in "${KEYLENS[@]}"; do
        run_one "5" "update_keylen${kl}" \
            "E5_update_keylen${kl}_${suffix}" \
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
# Experiment 6/7: YCSB A-F full sweep
# ============================================================================
exp6_ycsb_full() {
    local exp_id="$1"; local pool_gb="$2"; local profile_cflags="$3"
    local ycsb_tag="$4"; local map_frac="$5"
    echo ""
    echo "######################################################################"
    echo "# E${exp_id}: YCSB A-F @ pool=${pool_gb}GB, map_frac=${map_frac}, ops=${E6_E7_YCSB_OPS}  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "${profile_cflags}" || return 1
    local pool=$((pool_gb * NR_G))
    local suffix
    suffix="$(pick_variant_suffix)"
    for w in a b c d e f; do
        # YCSB 'e' uses a smaller op count (see E6_E7_YCSB_OPS_E) to keep
        # its range-scan footprint within the binary's tracking range.
        local w_ops="${E6_E7_YCSB_OPS}"
        if [[ "${w}" == "e" ]]; then
            w_ops="${E6_E7_YCSB_OPS_E}"
        fi
        run_one "${exp_id}" "ycsb_${w}_pool${pool_gb}G_${ycsb_tag}" \
            "E${exp_id}_ycsb_${w}_pool${pool_gb}G_${ycsb_tag}_${suffix}" \
            --pool_size "${pool}" \
            --ycsb "${w}" \
            --ycsb_ops "${w_ops}" \
            --map_size_frac "${map_frac}" \
            --keylen_mean 32 \
            --variable_keylen
    done
}

# ============================================================================
# Experiment 8: GC wear (10 GB pool, zipfian update)
# ============================================================================
exp8_gc_wear() {
    echo ""
    echo "######################################################################"
    echo "# E8: GC Wear (pool=${GC_WEAR_POOL_GB}GB, zipfian update)  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E8)" || return 1
    local suffix
    suffix="$(pick_variant_suffix)"
    run_one "8" "gc_wear" "E8_gc_wear_${suffix}" \
        --pool_size "${GC_WEAR_POOL}" \
        --map_size_frac 1.0000 \
        --num_update "${UPDATE_NUM_GC}" \
        --update_dist zipfian \
        --update_bench \
        --ssd_lat_off \
        --seed 1
}

# ============================================================================
# Experiment 9: Perf profile (manual attach)
# ============================================================================
exp9_perf_profile() {
    if [[ "${SKIP_PERF:-0}" == "1" ]]; then
        echo "[E9] SKIP_PERF=1, skipping perf profile"
        return 0
    fi
    echo ""
    echo "######################################################################"
    echo "# E9: Perf Profile (YCSB A, manual perf attach)  [${VARIANT_MODE_FLAG}]"
    echo "######################################################################"
    build_aggrekv "$(pick_variant_cflags E9)" || return 1
    local pool=$((64 * NR_G))
    local suffix
    suffix="$(pick_variant_suffix)"
    local frac
    frac="$(pick_variant_frac_fixed 0.2500)"
    local log_file="${LOG_DIR}/E9_perf_ycsb_a_${suffix}.log"
    local perf_data="${LOG_DIR}/perf.data"
    "${BIN}" --experiment_tag "perf_ycsb_a" \
        --pool_size "${pool}" \
        --map_size_frac "${frac}" \
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
# Summary: print stats to stdout; no aggregate file written (Functional badge only)
# ============================================================================
summarize_all() {
    # Functional badge: do not write aggregate summary files. Per-experiment
    # numbers stay inside each log via [SUMMARY] markers and are greppable
    # with `grep '\\[SUMMARY\\]' logs/*.log`.
    local txt=""
    local csv=""

    echo ""
    echo "========================================================================"
    echo " Hash-KVSSD (AggreKV) Test Suite — Summary (not written to disk)"
    echo "========================================================================"
    echo "Run started:  $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "NUMA node:    ${NUMA_NODE}"
    echo "SMOKE mode:   ${SMOKE_MODE}"
    echo "Variant:      ${VARIANT_MODE}  (aggrekv | rhik | all)"
    echo "Results dir:  ${RESULTS_DIR}"
    echo "========================================================================"

    # ----- E1: read bench (read_iops= / hit_rt=) -----
    echo ""
    echo "[E1] Read Throughput"
    echo "  Tag                              Variant    AvgIOPS   HitRate(%)"
    for f in "${LOG_DIR}"/E1_read_*_AggreKV.log "${LOG_DIR}"/E1_read_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local iops=$(grep -oP 'read_iops=\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'hit_rt=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-9s  %-9s  %s\n" "${tag}" "${variant}" "${iops:-N/A}" "${hit:-N/A}"
    done

    # ----- E2: update bench (update_iops=) -----
    echo ""
    echo "[E2] Update Throughput"
    echo "  Tag                              Variant    AvgIOPS"
    for f in "${LOG_DIR}"/E2_update_*_AggreKV.log "${LOG_DIR}"/E2_update_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local iops=$(grep -oP 'update_iops=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-9s  %s\n" "${tag}" "${variant}" "${iops:-N/A}"
    done

    # ----- E3: tail latency (avg_rlat/us: | p95_rlat/us: | ...) -----
    echo ""
    echo "[E3] Tail Latency (iodepth=1, zipfian)"
    echo "  Tag                              Variant    Avg(us)   p95(us)   p99(us)   p99.9(us)  p99.99(us)"
    for f in "${LOG_DIR}"/E3_tail_*_AggreKV.log "${LOG_DIR}"/E3_tail_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local line=$(grep -m1 "avg_rlat/us:" "$f" || true)
        if [[ -n "${line}" ]]; then
            local vals=$(echo "${line}" | awk -F'|' '{
                for (i=2; i<=6; i++) gsub(/[^0-9]/, "", $i);
                printf "%s %s %s %s %s\n", $2, $3, $4, $5, $6;
            }')
            printf "  %-32s  %-9s  %s\n" "${tag}" "${variant}" "${vals}"
        else
            printf "  %-32s  %-9s  N/A (no tail-latency line found)\n" "${tag}" "${variant}"
        fi
    done

    # ----- E4: YCSB keylen sensitivity -----
    echo ""
    echo "[E4] YCSB Key-Length Sensitivity"
    echo "  Tag                              Variant    KeyLen   TotalOps   AvgIOPS   AvgLat(us)  HitRate(%)"
    for f in "${LOG_DIR}"/E4_*_AggreKV.log "${LOG_DIR}"/E4_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local kl=$(echo "${tag}" | grep -oP 'keylen\K[0-9]+')
        local ops=$(grep -oP 'Total ops:\s*\K[\d.]+' "$f" | head -1)
        local iops=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "$f" | head -1)
        local lat=$(grep -oP 'Avg latency:\s*\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-9s  %-7s  %-9s  %-8s  %-10s  %s\n" \
            "${tag}" "${variant}" "${kl:-N/A}" "${ops:-N/A}" "${iops:-N/A}" "${lat:-N/A}" "${hit:-N/A}"
    done

    # ----- E5: Update bench keylen sensitivity -----
    echo ""
    echo "[E5] Update Key-Length Sensitivity"
    echo "  Tag                              Variant    KeyLen   UpdateNum   UpdateIOPS  HitRate(%)"
    for f in "${LOG_DIR}"/E5_*_AggreKV.log "${LOG_DIR}"/E5_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local kl=$(echo "${tag}" | grep -oP 'keylen\K[0-9]+')
        # update_bench SUMMARY line format:
        #   [SUMMARY] pool_size=..., update_dist=..., update_num=N, iodepth=..., update_iops=X.XX, hit_rt=Y.YYYY%
        local upd_num=$(grep -oP 'update_num=\K[0-9]+' "$f" | head -1)
        local upd_iops=$(grep -oP 'update_iops=\K[\d.]+' "$f" | head -1)
        local hit=$(grep -oP 'hit_rt=\K[\d.]+' "$f" | head -1)
        printf "  %-32s  %-9s  %-7s  %-9s  %-10s  %s\n" \
            "${tag}" "${variant}" "${kl:-N/A}" "${upd_num:-N/A}" "${upd_iops:-N/A}" "${hit:-N/A}"
    done

    # ----- E6/E7: YCSB A-F -----
    for exp in 6 7; do
        echo ""
        echo "[E${exp}] YCSB A-F"
        echo "  Tag                              Variant    Workload  TotalOps   AvgIOPS   AvgLat(us)  MaxLat(us)  HitRate(%)"
        for f in "${LOG_DIR}"/E${exp}_ycsb_*_AggreKV.log "${LOG_DIR}"/E${exp}_ycsb_*_RHIK.log; do
            [[ ! -f "$f" ]] && continue
            local variant="AggreKV"
            if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
            local tag=$(basename "$f" .log)
            local wl=$(echo "${tag}" | grep -oP 'ycsb_\K[a-f]')
            local ops=$(grep -oP 'Total ops:\s*\K[\d.]+' "$f" | head -1)
            local iops=$(grep -oP 'Avg IOPS:\s*\K[\d.]+' "$f" | head -1)
            local lat=$(grep -oP 'Avg latency:\s*\K[\d.]+' "$f" | head -1)
            local max=$(grep -oP 'Max latency:\s*\K[\d.]+' "$f" | head -1)
            local hit=$(grep -oP 'Cache Hit:\s*\K[\d.]+' "$f" | head -1)
            printf "  %-32s  %-9s  %-8s  %-9s  %-8s  %-10s  %-10s  %s\n" \
                "${tag}" "${variant}" "${wl:-N/A}" "${ops:-N/A}" "${iops:-N/A}" "${lat:-N/A}" "${max:-N/A}" "${hit:-N/A}"
        done
    done

    # ----- E8: GC wear (total GC erase count / sblk[N]: gc_cnt=) -----
    echo ""
    echo "[E8] GC Wear"
    echo "  Tag                              Variant    TotalErased   SBLKsWithData"
    for f in "${LOG_DIR}"/E8_*_AggreKV.log "${LOG_DIR}"/E8_*_RHIK.log; do
        [[ ! -f "$f" ]] && continue
        local variant="AggreKV"
        if [[ "$f" == *_RHIK.log ]]; then variant="RHIK"; fi
        local tag=$(basename "$f" .log)
        local total=$(grep "total GC erase count:" "$f" | sed 's/.*: //' | head -1)
        local sblk_n=$(grep -c "sblk\[" "$f" || true)
        printf "  %-32s  %-9s  %-12s  %s\n" "${tag}" "${variant}" "${total:-N/A}" "${sblk_n:-0}"
    done

    # ----- E9: perf profile -----
    if [[ -f "${LOG_DIR}/E9_perf_ycsb_a_AggreKV.log" || -f "${LOG_DIR}/E9_perf_ycsb_a_RHIK.log" ]]; then
        echo ""
        echo "[E9] Perf Profile (YCSB A)"
        echo "  Log: ${LOG_DIR}/E9_perf_ycsb_a_AggreKV.log (or _RHIK.log)"
        echo "  Perf data: ${LOG_DIR}/perf.data"
    fi

    echo ""
    echo "========================================================================"
    echo "Finished at: $(date)"
    echo "========================================================================"
    echo ""
    echo "Aggregate summary: not written to disk (Functional badge only)."
    echo "Per-experiment numbers are available via: grep '\\[SUMMARY\\]' \"${LOG_DIR}\"/*.log"
}

# ============================================================================
# Main
# ============================================================================
echo "================================================"
echo " Hash-KVSSD (AggreKV) All-in-One Test Suite"
echo "================================================"
echo "Results dir: ${RESULTS_DIR}"
echo "NUMA node:   ${NUMA_NODE}"
echo "SMOKE mode:  ${SMOKE_MODE}"
echo "Variant:     ${VARIANT_MODE}  (aggrekv | rhik | all)"
if [[ ${#ONLY_EXPS[@]} -gt 0 ]]; then
    echo "Only run:    ${ONLY_EXPS[*]}"
fi
echo "================================================"


run_variant() {
    local variant_label="$1"
    export VARIANT_MODE_FLAG="${variant_label}"

    if [[ "${VARIANT_MODE}" != "all" && "${VARIANT_MODE}" != "${variant_label,,}" ]]; then
        return 0
    fi

    echo ""
    echo "================================================================"
    echo "  Running variant: ${variant_label}"
    echo "================================================================"

    should_run "E1" && exp1_read_throughput    || echo "[skip E1 ${variant_label}]"
    should_run "E2" && exp2_update_throughput  || echo "[skip E2 ${variant_label}]"
    should_run "E3" && exp3_tail_latency       || echo "[skip E3 ${variant_label}]"
    should_run "E4" && exp4_ycsb_keylen        || echo "[skip E4 ${variant_label}]"
    should_run "E5" && exp5_update_keylen      || echo "[skip E5 ${variant_label}]"
    should_run "E6" && exp6_ycsb_full 6 "${E6_POOL_GB}" "$(pick_variant_cflags E6)" "E6" "$(pick_variant_frac_fixed ${E6_MAP_FRAC})" || echo "[skip E6 ${variant_label}]"
    should_run "E7" && exp6_ycsb_full 7 "${E7_POOL_GB}" "$(pick_variant_cflags E7)" "E7" "$(pick_variant_frac_fixed ${E7_MAP_FRAC})" || echo "[skip E7 ${variant_label}]"
    should_run "E8" && exp8_gc_wear            || echo "[skip E8 ${variant_label}]"
    should_run "E9" && exp9_perf_profile       || echo "[skip E9 ${variant_label}]"
}

run_variant "AggreKV"
run_variant "RHIK"

unset VARIANT_MODE_FLAG

echo ""
echo "================================================"
echo " Summarizing..."
echo "================================================"
summarize_all

# Functional badge: drop the per-binary CSV files that
# emit_read_bench_summary / emit_update_bench_summary write into
# hash_kvssd_results/ (see hash_kvssd/test_ext_mem_lat.c). The
# numbers they carry are still inside each log via [SUMMARY] lines.
if [[ -d "${RESULTS_DIR}" ]]; then
    find "${RESULTS_DIR}" -maxdepth 1 -type f \
        \( -name 'read_test_summary.csv' -o -name 'update_test_summary.csv' \) \
        -delete 2>/dev/null || true
fi

echo ""
echo "All logs:     ${LOG_DIR}/"
echo "Summary:      (no aggregate file written; Functional badge only)"
