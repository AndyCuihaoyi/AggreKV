#!/usr/bin/env bash
# ============================================================================
#  AggreKV Artifact — Blktrace Dataset Downloader
# ----------------------------------------------------------------------------
#  Downloads the 44-file blktrace dataset (22 csv + 22 cnt; ≈ 6.12 GB raw)
#  from the AggreKV Zenodo dataset record into ./block_ssd/blktrace/.
#
#  Zenodo record: https://zenodo.org/records/21455311
#  Zenodo DOI:    10.5281/zenodo.21455311
#
#  After downloading, the script verifies every file against the SHA256
#  manifest embedded below (computed from the authors' upload on 2026-07-21)
#  and exits non-zero on any mismatch.
#
#  Re-running this script is safe:
#    - 4 parallel wget jobs (PARALLEL=4 by default; override as
#      PARALLEL=8 bash download-blktrace.sh)
#    - each wget uses -nc (no-clobber); partial files are resumed on retry
#    - already-correct files are skipped after a successful sha256 verify
#
#  Tested with wget 1.21 + bash 5.x + sha256sum on Ubuntu 22.04 LTS.
# ============================================================================
set -euo pipefail

ZENODO_RECORD="21455311"
DEST="block_ssd/blktrace"
PARALLEL="${PARALLEL:-4}"

mkdir -p "${DEST}"

# ----------------------------------------------------------------------------
# 44 file URLs (sorted). The 2-tuple is (URL, FILENAME); wget downloads by URL
# but writes to FILENAME so the basename layout matches block_ssd/blktrace/.
# ----------------------------------------------------------------------------
URLS=(
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_128000000_500000x8.cnt/content" "rocksdb_overwrite_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_128000000_500000x8.csv/content" "rocksdb_overwrite_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_200000000_500000x8.cnt/content" "rocksdb_overwrite_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_200000000_500000x8.csv/content" "rocksdb_overwrite_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_64000000_500000x8.cnt/content" "rocksdb_overwrite_64000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwrite_64000000_500000x8.csv/content" "rocksdb_overwrite_64000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_128000000_500000x8.cnt/content" "rocksdb_overwritezipf_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_128000000_500000x8.csv/content" "rocksdb_overwritezipf_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_200000000_500000x8.cnt/content" "rocksdb_overwritezipf_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_200000000_500000x8.csv/content" "rocksdb_overwritezipf_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_64000000_500000x8.cnt/content" "rocksdb_overwritezipf_64000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_overwritezipf_64000000_500000x8.csv/content" "rocksdb_overwritezipf_64000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_128000000_500000x8.cnt/content" "rocksdb_readrandom_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_128000000_500000x8.csv/content" "rocksdb_readrandom_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_200000000_500000x8.cnt/content" "rocksdb_readrandom_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_200000000_500000x8.csv/content" "rocksdb_readrandom_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_64000000_500000x8.cnt/content" "rocksdb_readrandom_64000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readrandom_64000000_500000x8.csv/content" "rocksdb_readrandom_64000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_128000000_1250000x8.cnt/content" "rocksdb_readzipf_128000000_1250000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_128000000_1250000x8.csv/content" "rocksdb_readzipf_128000000_1250000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_200000000_1250000x8.cnt/content" "rocksdb_readzipf_200000000_1250000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_200000000_1250000x8.csv/content" "rocksdb_readzipf_200000000_1250000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_64000000_1250000x8.cnt/content" "rocksdb_readzipf_64000000_1250000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_readzipf_64000000_1250000x8.csv/content" "rocksdb_readzipf_64000000_1250000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-a_128000000_500000x8.cnt/content" "rocksdb_ycsb-a_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-a_128000000_500000x8.csv/content" "rocksdb_ycsb-a_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-a_200000000_500000x8.cnt/content" "rocksdb_ycsb-a_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-a_200000000_500000x8.csv/content" "rocksdb_ycsb-a_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-b_128000000_500000x8.cnt/content" "rocksdb_ycsb-b_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-b_128000000_500000x8.csv/content" "rocksdb_ycsb-b_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-b_200000000_500000x8.cnt/content" "rocksdb_ycsb-b_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-b_200000000_500000x8.csv/content" "rocksdb_ycsb-b_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-d_128000000_500000x8.cnt/content" "rocksdb_ycsb-d_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-d_128000000_500000x8.csv/content" "rocksdb_ycsb-d_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-d_200000000_500000x8.cnt/content" "rocksdb_ycsb-d_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-d_200000000_500000x8.csv/content" "rocksdb_ycsb-d_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-e_128000000_500000x8.cnt/content" "rocksdb_ycsb-e_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-e_128000000_500000x8.csv/content" "rocksdb_ycsb-e_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-e_200000000_500000x8.cnt/content" "rocksdb_ycsb-e_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-e_200000000_500000x8.csv/content" "rocksdb_ycsb-e_200000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-f_128000000_500000x8.cnt/content" "rocksdb_ycsb-f_128000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-f_128000000_500000x8.csv/content" "rocksdb_ycsb-f_128000000_500000x8.csv"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-f_200000000_500000x8.cnt/content" "rocksdb_ycsb-f_200000000_500000x8.cnt"
    "https://zenodo.org/api/records/21455311/files/rocksdb_ycsb-f_200000000_500000x8.csv/content" "rocksdb_ycsb-f_200000000_500000x8.csv"
)

# ----------------------------------------------------------------------------
# SHA256 manifest (44 entries, sorted)
# ----------------------------------------------------------------------------
SHA256_MANIFEST='
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwrite_128000000_500000x8.cnt
b4389d9764350c59722a846b89963ffb715099447b9f68c5fe9bb7f9a133a5c4  rocksdb_overwrite_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwrite_200000000_500000x8.cnt
89888039726e2cd25379cd029bda2b19a37194bba3d1c0d0e128afa6998834a6  rocksdb_overwrite_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwrite_64000000_500000x8.cnt
434ea2bff6ec9f99ab0bc0e3f0b63c27d64a659d4be37f8ac7d45bea3cc49e92  rocksdb_overwrite_64000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwritezipf_128000000_500000x8.cnt
fbd76e6ebf32a460a138346f7ae648f35543a8151f5f14d30aab3935a3030734  rocksdb_overwritezipf_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwritezipf_200000000_500000x8.cnt
32fd10259702df63956ece93d7f42e970d56ad12ed345451ab76d56dc066bc63  rocksdb_overwritezipf_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_overwritezipf_64000000_500000x8.cnt
565d13532abe7d417b8aca31455b32d264ddc8b266f612596d7cf15b11653650  rocksdb_overwritezipf_64000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_readrandom_128000000_500000x8.cnt
688b7288fca7ffacb1ef44cd12de2c165f7f01a4f38ba961dc759e15a3c8ab5d  rocksdb_readrandom_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_readrandom_200000000_500000x8.cnt
1536d5760ac654e2896aec63946d7df9a0abd2a6b951ba48345fe5c639f30c0f  rocksdb_readrandom_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_readrandom_64000000_500000x8.cnt
291cf90b3730956536afe4a7c5f43604ed8c90a6e07b5577c3b44d9e1437ac45  rocksdb_readrandom_64000000_500000x8.csv
de6aeb89b0d91519a443ac503ea9e652f130752e5ecc78cbcffc3e0f04e4bbf0  rocksdb_readzipf_128000000_1250000x8.cnt
79ef11fac2652ae5630858105044803638f6e73bc83517bb79958deb34d3d96f  rocksdb_readzipf_128000000_1250000x8.csv
de6aeb89b0d91519a443ac503ea9e652f130752e5ecc78cbcffc3e0f04e4bbf0  rocksdb_readzipf_200000000_1250000x8.cnt
217a415061a2bc3c5b517eeba9cfe48d58916911b041447bbad482e25101ddb7  rocksdb_readzipf_200000000_1250000x8.csv
de6aeb89b0d91519a443ac503ea9e652f130752e5ecc78cbcffc3e0f04e4bbf0  rocksdb_readzipf_64000000_1250000x8.cnt
fea03a3014d40c030ad92fe0d051a7db587ebc53f619028a1427be8ddc3d5f07  rocksdb_readzipf_64000000_1250000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-a_128000000_500000x8.cnt
0af50faeb5e7a6c13617df9122d0aec9a3202811b306cb9b94d9e5724fd07f1d  rocksdb_ycsb-a_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-a_200000000_500000x8.cnt
60c115138700ff25644c7b063f6f2128664c088fc95a544cbd9caf623e3ea8dd  rocksdb_ycsb-a_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-b_128000000_500000x8.cnt
16a9a8b1befdb9414eb447890cd4d2e92e240ab06e49d77431a6c02d0ee9fca4  rocksdb_ycsb-b_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-b_200000000_500000x8.cnt
de984d41b7a6b4980ea9597213d8aa45d24063b3f2a699fa7bfb9cad63ec1b84  rocksdb_ycsb-b_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-d_128000000_500000x8.cnt
48a80e5b885561f6baef1ac1bcf931afceeaa07cc2a2a1b143e572a409bf6682  rocksdb_ycsb-d_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-d_200000000_500000x8.cnt
d6e970bc157bcce61072986a3be6e3ed4229079d91b54f72139895053ee8c34b  rocksdb_ycsb-d_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-e_128000000_500000x8.cnt
aff1e42d2580ab02f79801bd3cb31b5ed54810cbb079e6582260f8276ecbb9c5  rocksdb_ycsb-e_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-e_200000000_500000x8.cnt
8bedaabb301f81c2018d2715520ba1f77242e9bd1b3a42604a50410d69de6e05  rocksdb_ycsb-e_200000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-f_128000000_500000x8.cnt
129739e3fc0564190f9596cefe31a51ffc053f52a8cff0c0a63e129e434589eb  rocksdb_ycsb-f_128000000_500000x8.csv
a92373742eac3ba9e17aa3c09b8bcb19aefda9be4f665a6a94048ab1ae6247c8  rocksdb_ycsb-f_200000000_500000x8.cnt
882e0ec96f41ff80af54853fe2746f4fa1ab2531ea206ba488ef2a5e4ef31337  rocksdb_ycsb-f_200000000_500000x8.csv
'

# ----------------------------------------------------------------------------
# Verify wget + sha256sum availability
# ----------------------------------------------------------------------------
for tool in wget sha256sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] '$tool' not found. Install it with: sudo apt install wget coreutils" >&2
        exit 1
    fi
done

echo ""
echo "=================================================================="
echo "  AggreKV blktrace dataset downloader"
echo "    Source:    Zenodo record ${ZENODO_RECORD} (DOI 10.5281/zenodo.${ZENODO_RECORD})"
echo "    Target:    ${DEST}/"
echo "    Files:     44 (parallel = ${PARALLEL})"
echo "    Estimated: ~6.12 GB raw"
echo "=================================================================="
echo ""

# ----------------------------------------------------------------------------
# Download phase — parallel wget. Each "URL FILENAME" pair invokes wget with
# -O to write the file under its intended basename. DEST is exported into the
# sub-shell so xargs can find the target directory.
# ----------------------------------------------------------------------------
TMP_URL_LIST="$(mktemp)"
trap 'rm -f "${TMP_URL_LIST}"' EXIT

for entry in "${URLS[@]}"; do
    echo "${entry}"
done > "${TMP_URL_LIST}"

export DEST
xargs -P "${PARALLEL}" -n 2 bash -c '
    url="$1"
    name="$2"
    out="${DEST}/${name}"
    if [[ ! -f "${out}" ]] || [[ ! -s "${out}" ]]; then
        wget -nc -q --tries=5 --retry-connrefused --timeout=60 \
             -O "${out}" "${url}"
    fi
' _ < "${TMP_URL_LIST}"

rm -f "${TMP_URL_LIST}"
trap - EXIT

# ----------------------------------------------------------------------------
# Verify phase — sha256 check on every file
# ----------------------------------------------------------------------------
echo ""
echo "=================================================================="
echo "  Verifying SHA256 checksums..."
echo "=================================================================="

verify_failed=0
while IFS= read -r line; do
    [[ -z "${line}" ]] && continue
    expected_sha=$(echo "${line}" | awk '{ print $1 }')
    base=$(echo "${line}" | awk '{ print $2 }')
    [[ -z "${base}" ]] && continue
    local_file="${DEST}/${base}"
    if [[ ! -f "${local_file}" ]]; then
        echo "  [MISSING]   ${base}" >&2
        verify_failed=1
        continue
    fi
    actual_sha=$(sha256sum "${local_file}" | awk '{ print $1 }')
    if [[ "${actual_sha}" == "${expected_sha}" ]]; then
        printf "  [OK] %s\n" "${base}"
    else
        echo "  [MISMATCH]  ${base}  (expected ${expected_sha}, got ${actual_sha})" >&2
        verify_failed=1
    fi
done <<< "${SHA256_MANIFEST}"

echo ""
if [[ "$verify_failed" -ne 0 ]]; then
    echo "[FAIL] One or more files failed verification." >&2
    echo "        Re-run this script; partial files will resume." >&2
    exit 1
fi

CSV_COUNT=$(ls "${DEST}"/*.csv 2>/dev/null | wc -l)
CNT_COUNT=$(ls "${DEST}"/*.cnt 2>/dev/null | wc -l)
echo "[OK] All 44 files verified."
echo "    ${CSV_COUNT} .csv files"
echo "    ${CNT_COUNT} .cnt files"
echo "    Ready to build the block_ssd sub-module. See INSTALL.md §3.2."
