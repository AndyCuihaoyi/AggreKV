# INSTALL

This document walks through installing OS dependencies, obtaining the source, and building the three sub-modules on a fresh Linux x86_64 box so that `bash test.sh` can run from the repository root. Hardware / NUMA / RAM constraints live in `REQUIREMENTS.md`.

**Sections in order:**

1. OS-level dependencies.
2. `git clone` the source.
3. Prepare the Block-SSD baseline (download 44 blktrace files via `download-blktrace.sh`, build `block_ssd`, verify).
4. First run (`bash test.sh`).
5. Troubleshooting.

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

---

## 2. Obtain the source

```bash
git clone https://github.com/AndyCuihaoyi/AggreKV.git AggreKV
cd AggreKV
```

After cloning, the top of the tree looks like:

```
AggreKV/
├── README.md
├── STATUS.md
├── REQUIREMENTS.md
├── INSTALL.md
├── LICENSE
├── test.sh
├── hash_kvssd/
├── lsmtree_kvssd/
└── block_ssd/
```
---

## 3. Prepare the Block-SSD baseline

This section does three things in order: fetch the 6.2 GB blktrace dataset, build the `block_ssd` sub-module, and verify the expected binaries exist. The `hash_kvssd` and `lsmtree_kvssd` sub-modules do **not** need any pre-build — their sub-drivers build themselves on first invocation — so they are not touched in this section.

### 3.1 Download the blktrace dataset

The `block_ssd` baseline replays 22 RocksDB-derived blktrace CSV files (≈ 6.2 GB). The dataset is not in the GitHub repo; it is published on Zenodo as a separate **Dataset** record.

- Zenodo record: <https://zenodo.org/records/21455311>
- Zenodo DOI: <https://doi.org/10.5281/zenodo.21455311>

The record ships the 44 files as **individual assets** (22 `.csv` + 22 `.cnt`, total 6.12 GB). The repo ships a one-shot downloader that puts them under `block_ssd/blktrace/` with stable filenames:

```bash
# From the repository root
bash download-blktrace.sh
```


To re-confirm the layout:

```bash
ls block_ssd/blktrace/ | wc -l      # expect 44
ls block_ssd/blktrace/*.csv | wc -l # expect 22
ls block_ssd/blktrace/*.cnt | wc -l # expect 22
```

### 3.2 Build the `block_ssd` sub-module

The `block_ssd` sub-driver does **not** build its own binary; this step is required before `test.sh` can complete the third step.

```bash
( cd block_ssd && make test_blktrace test_ext_mem_lat )
```

This produces two binaries under `block_ssd/dftl_block/`:

- `test_blktrace` — used by `run_blktrace_tests.sh`
- `test_ext_mem_lat` — also built so the repo ships the full set of binaries

### 3.3 Verify build success

The two binaries below **must** both exist at the end of §3.2:

```bash
test -x block_ssd/dftl_block/test_blktrace \
   -a -x block_ssd/dftl_block/test_ext_mem_lat \
&& echo "[INSTALL OK] block_ssd binaries built — proceed to test.sh" \
|| echo "[INSTALL FAIL] one or more block_ssd binaries missing — re-run the make command above"
```

---

## 4. First run

From the repository root, invoke the orchestrator with no arguments so each sub-driver uses its own defaults:

```bash
bash test.sh
```

This runs the validation experiments of all three sub-modules to completion:

- `hash_kvssd` runs nine experiment blocks (E1–E9), forcing a clean rebuild at the start of each one so the per-experiment CFLAGS take effect. Logs land under `hash_kvssd/hash_kvssd_results/`.
- `lsmtree_kvssd` iterates its built-in workload set with the default key count. Logs land under `lsmtree_kvssd/lsmtree_test_results/`.
- `block_ssd` replays every `.csv` paired with its same-name `.cnt` under `block_ssd/blktrace/`. Logs land under `block_ssd/blktrace_test_results/`.

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
| `bash test.sh` fails at step 3 (`block_ssd`) | missing binary or missing dataset | If `test_blktrace` is missing, re-run §3.2. If the dataset is missing, fetch it via §3.1. |
| `bash test.sh` killed by the kernel (OOM) | RAM less than `REQUIREMENTS.md` §"Hardware Environment" recommends | The simulator may be swapping; check `vmstat 1` while a run is in progress. The run may still complete, but numerical results will not match the paper in this case. |
| `Invalid cnt file content` in `run_blktrace_tests.sh` | blktrace extraction did not land `*.csv` and `*.cnt` side-by-side in `block_ssd/blktrace/` | Re-extract the dataset so both extensions sit in the same directory. |
| AddressSanitizer errors during a run | an `_asan` target was built instead of the default one | ASan targets (`*_asan` in each sub-module's `Makefile`) are not used by `test.sh`. If you see ASan output, double-check the build command in §3.2 is exactly `make test_blktrace test_ext_mem_lat`. |
