# INSTALL

This document walks a reviewer from a fresh Linux x86_64 box to a fully built artifact that is ready to run `bash test.sh`. It does not restate the hardware / NUMA / RAM constraints — those are in `REQUIREMENTS.md`.

**Sections in order:**

1. OS-level dependencies (`apt install`).
2. `git clone` the artifact.
3. Prepare the Block-SSD baseline (download 44 blktrace files via `download-blktrace.sh`, build `block_ssd`, verify).
4. First run (`bash test.sh`).
5. Troubleshooting.

If the reviewer does not need the Block-SSD baseline, §3 can be skipped entirely — `test.sh` will then run only the `hash_kvssd` and `lsmtree_kvssd` baselines.

---

## 1. Install OS-level dependencies

```bash
sudo apt update
sudo apt install -y build-essential libglib2.0-dev numactl
```

| Package               | Why                                                                                  |
|-----------------------|--------------------------------------------------------------------------------------|
| `build-essential`     | `gcc` + `make` + headers                                                             |
| `libglib2.0-dev`      | Required by the `hash_kvssd` baseline (its build script hard-codes the glib headers path) |
| `numactl`             | Optional; if absent, the `hash_kvssd` sub-driver falls back to `taskset -c 0`        |

If `numactl` is not available in your distribution, omit it from the line above; the `hash_kvssd` sub-driver will print a warning and fall back to `taskset -c 0`.

---

## 2. Obtain the source

```bash
git clone <TODO: GitHub repo URL> aggrekv-artifact
cd aggrekv-artifact
```

After cloning, the top of the tree looks like:

```
aggrekv-artifact/
├── README.md
├── STATUS.md
├── REQUIREMENTS.md
├── INSTALL.md          ← this file
├── LICENSE
├── test.sh
├── hash_kvssd/
├── lsmtree_kvssd/
└── block_ssd/
```

The persistent, DOI-indexed archive that the CASES submission form references is the Zenodo record of this GitHub repo. The DOI placeholder will be filled in once the Zenodo record is published; the DOI is recorded in `STATUS.md` §"Artifacts Available".

---

## 3. Prepare the Block-SSD baseline

This section does three things in order: fetch the 6.2 GB blktrace dataset, build the `block_ssd` sub-module, and verify the two expected binaries exist. The `hash_kvssd` and `lsmtree_kvssd` sub-modules do **not** need any pre-build — their sub-drivers build themselves on first invocation — so they are not touched in this section.

### 3.1 Download the blktrace dataset

The `block_ssd` baseline replays 22 RocksDB-derived blktrace CSV files (≈ 6.2 GB). The dataset is not in the GitHub repo; it is published on Zenodo as a separate **Dataset** record.

- Zenodo record: <https://zenodo.org/records/21455311>
- Zenodo DOI: <https://doi.org/10.5281/zenodo.21455311>

The record ships the 44 files as **individual assets** (22 `.csv` + 22 `.cnt`, total 6.12 GB). The artifact ships a one-shot downloader that puts them under `block_ssd/blktrace/` with stable filenames:

```bash
# From the repository root
bash download-blktrace.sh
```

The script:

- runs **4 `wget` jobs in parallel** (override with `PARALLEL=8 bash download-blktrace.sh`);
- uses `wget -nc` so re-runs are safe — partial files are resumed and intact files are not redownloaded;
- **verifies the SHA256 of every file** against an embedded manifest after download; any mismatch causes the script to exit non-zero (re-running resumes partial transfers).

A successful run prints `[OK] All 44 files verified.` and exits 0. To re-confirm the layout:

```bash
ls block_ssd/blktrace/ | wc -l      # expect 44
ls block_ssd/blktrace/*.csv | wc -l # expect 22
ls block_ssd/blktrace/*.cnt | wc -l # expect 22
```

Each `.csv` must be paired with a same-name `.cnt` file (e.g. `rocksdb_overwrite_64000000_500000x8.csv` and `rocksdb_overwrite_64000000_500000x8.cnt`); `run_blktrace_tests.sh` reads the `.cnt` for `num_upper_level_kvops`.

> If `wget` is not installed, install it with `sudo apt install wget`. The script does not require authentication and does not need an API token — the Zenodo record is fully public.

If you do not want to run the Block-SSD baseline, skip this whole subsection. `run_blktrace_tests.sh` will detect that `block_ssd/blktrace/` is empty and exit 0 with a clear skip message — this is expected behaviour, not a failure, and the Functional badge still passes on the `hash_kvssd` + `lsmtree_kvssd` runs.

### 3.2 Build the `block_ssd` sub-module

The `block_ssd` sub-driver does **not** build its own binary; this step is required before `test.sh` can complete the third step.

```bash
( cd block_ssd && make test_blktrace test_ext_mem_lat )
```

This produces two binaries under `block_ssd/dftl_block/`:

- `test_blktrace` — used by `run_blktrace_tests.sh`
- `test_ext_mem_lat` — also built so the artifact ships the full set of binaries

### 3.3 Verify build success

The two binaries below **must** both exist at the end of §3.2:

```bash
test -x block_ssd/dftl_block/test_blktrace \
   -a -x block_ssd/dftl_block/test_ext_mem_lat \
&& echo "[INSTALL OK] block_ssd binaries built — proceed to test.sh" \
|| echo "[INSTALL FAIL] one or more block_ssd binaries missing — re-run the make command above"
```

A passing run prints:

```
[INSTALL OK] block_ssd binaries built — proceed to test.sh
```

---

## 4. First run

From the repository root:

```bash
bash test.sh
```

`test.sh` invokes the three sub-drivers in order. If everything succeeds, it prints:

```
[TEST OK] all three sub-drivers finished
```

and exits 0. If you skipped §3.1 (no blktrace dataset), the third step will instead print a multi-line skip message that begins with:

```
[block_ssd] blktrace dataset missing under ./blktrace/
[block_ssd] To run the Block-SSD baseline, fetch the Zenodo archive ...
[block_ssd] Skipping Block-SSD baseline (Functional badge does not require this step).
```

This is not a failure — the Functional run is still considered passing on the `hash_kvssd` + `lsmtree_kvssd` runs alone.

`hash_kvssd` rebuilds itself at the start of each of its experiments (it `make clean`s and recompiles with the per-experiment CFLAGS), so the overall `test.sh` run can take a while; the per-experiment logs land under `hash_kvssd/hash_kvssd_results/`.

---

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `apt` cannot find `libglib2.0-dev` | distro is not Debian / Ubuntu family | `apt` is the Ubuntu/Debian command; on other distros use the equivalent (e.g. `dnf install glib2-devel` on Fedora, `pacman -S glib2` on Arch). |
| `block_ssd` build fails with `glib.h: No such file or directory` | `libglib2.0-dev` not installed | Re-run `sudo apt install libglib2.0-dev` and retry §3.2. |
| `hash_kvssd` build fails with `glib.h: No such file or directory` even after installing `libglib2.0-dev` | glib headers live at a different path on this distro | The `hash_kvssd` build script hard-codes `/usr/include/glib-2.0` and `/usr/lib/x86_64-linux-gnu/glib-2.0/include/` (Debian/Ubuntu x86_64 paths). On Fedora the glibconfig path is typically `/usr/lib64/glib-2.0/include/`; the build script would need to be edited to point at the correct path. |
| `block_ssd` build fails with a gcc error | gcc is older than 9.4 | See `REQUIREMENTS.md` §"Software Environment". |
| `bash test.sh` fails at step 1 (`hash_kvssd`) | gcc too old, or missing build dep | Inspect `hash_kvssd/hash_kvssd_results/logs/` for the failing experiment's log; the first compiler error is the root cause. |
| `bash test.sh` fails at step 2 (`lsmtree_kvssd`) | build or runtime failure in `lsm_test` | Inspect `lsmtree_kvssd/lsmtree_test_results/` for the failing workload's log. |
| `bash test.sh` fails at step 3 (`block_ssd`) | missing binary or missing dataset | If `test_blktrace` is missing, re-run §3.2. If the dataset is missing, fetch it via §3.1 — or accept the skip message and consider the Functional run as passing on the first two baselines. |
| `bash test.sh` killed by the kernel (OOM) | RAM less than `REQUIREMENTS.md` §"Hardware Environment" recommends | The simulator may be swapping; check `vmstat 1` while a run is in progress. The Functional badge still passes on a non-zero exit (the run is "exercised and produces well-formed output" up to the OOM), but numerical alignment with the paper is not expected in this case. |
| `Invalid cnt file content` in `run_blktrace_tests.sh` | blktrace extraction did not land `*.csv` and `*.cnt` side-by-side in `block_ssd/blktrace/` | Re-extract the dataset so both extensions sit in the same directory. |
| AddressSanitizer errors during a run | an `_asan` target was built instead of the default one | ASan targets (`*_asan` in each sub-module's `Makefile`) are not used by `test.sh`. If you see ASan output, double-check the build command in §3.2 is exactly `make test_blktrace test_ext_mem_lat`. |
