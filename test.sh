#!/usr/bin/env bash
# ============================================================================
#  AggreKV Artifact — Thin Entry Point
# ----------------------------------------------------------------------------
#  This script is intentionally minimal: it only `cd`s into each of the three
#  sub-modules and runs that sub-module's own driver, in order.
#
#  To run a sub-module with non-default arguments, invoke that sub-module's driver
#  directly (the sub-driver headers document their own flag catalogs).
#
#  Exit code: 0 if all three sub-drivers exit 0; otherwise the failing
#  sub-driver's exit code (and the script aborts at the first failure).
# ============================================================================
set -euo pipefail

ARTIFACT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "${ARTIFACT_ROOT}"

run_subdriver() {
    local subdir="$1"; local driver="$2"
    echo ""
    echo "================================================================"
    echo "  [sub] ${subdir}/${driver}"
    echo "================================================================"
    (
        cd "${ARTIFACT_ROOT}/${subdir}"
        bash "./${driver}"
    )
}

run_subdriver "hash_kvssd"    "run_all_hashkvssd_tests.sh"
run_subdriver "lsmtree_kvssd" "run_lsmtree_tests.sh"
run_subdriver "block_ssd"     "run_blktrace_tests.sh"

echo ""
echo "[TEST OK] all three sub-drivers finished"
