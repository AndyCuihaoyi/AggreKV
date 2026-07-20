# INSTALL

This document walks a reviewer from a fresh Linux box to a fully built artifact that is ready to run `test.sh`.

---

## 1. Install OS-level dependencies

```bash
sudo apt update
sudo apt install -y build-essential libglib2.0-dev numactl
```

| Package               | Why                                                |
|-----------------------|----------------------------------------------------|
| `build-essential`     | `gcc` + `make` + headers                           |
| `libglib2.0-dev`      | Required by `hash_kvssd` and `block_ssd` baselines |
| `numactl`             | Optional; NUMA-binding for `numactl --membind 0 …` |

If `numactl` is unavailable in your distro, that is fine — every script in this artifact auto-falls-back to `taskset -c 0` and logs a warning.

---

## 2. Obtain the artifact

The artifact is published as a single Zenodo archive containing the source tree, the `paper/` PDF, and the bundled `block_ssd/blktrace/` traces (≈6.2 GB on disk after extraction). Download and extract it:

```bash
# Pick a working directory anywhere you have ~10 GB free.
mkdir aggrekv-work && cd aggrekv-work
curl -L -o aggrekv-artifact.tar.gz  "<Zenodo-artifact-file-URL>"
tar -xzf aggrekv-artifact.tar.gz
cd AggreKV-artifact
```

(The single `<Zenodo-artifact-file-URL>` placeholder will be filled in once the Zenodo record is published; see `STATUS.md` §2 for the DOI and badge-applied-on date.)

> **Distribution channel.** Zenodo is the only channel advertised to reviewers; no GitHub mirror is published. You do not need git to use this artifact.

---

## 3. Build all sub-modules

The three sub-modules are independent; you can build them in any order.

```bash
# 3.1 AggreKV (hash_kvssd) — produces the test_ext_mem_lat binary
( cd hash_kvssd && make test_ext_mem_lat )

# 3.2 Block-SSD baseline — produces test_ext_mem_lat + test_blktrace
( cd block_ssd  && make test_blktrace test_ext_mem_lat )

# 3.3 LSM-tree baseline — produces lsm_test
( cd lsmtree_kvssd && make lsm_test )
```

---

## 4. Verify build success

The four binaries below **must all exist** at the end of step 3.

```bash
ls -1 hash_kvssd/hash_hot_cmt/test_ext_mem_lat \
       block_ssd/dftl_block/test_ext_mem_lat \
       block_ssd/dftl_block/test_blktrace \
       lsmtree_kvssd/lsmtree/lsm_test
```

**Success marker (copy-paste this line; reviewers confirm by running it):**

```bash
test -x hash_kvssd/hash_hot_cmt/test_ext_mem_lat \
   -a -x block_ssd/dftl_block/test_ext_mem_lat \
   -a -x block_ssd/dftl_block/test_blktrace \
   -a -x lsmtree_kvssd/lsmtree/lsm_test \
&& echo "[INSTALL OK] all four binaries built — proceed to test.sh" \
|| echo "[INSTALL FAIL] one or more binaries missing — re-run the corresponding make command above"
```

A passing run prints:

```
[INSTALL OK] all four binaries built — proceed to test.sh
```

---

## 5. Sanity check

After the four required binaries are present, run the top-level entry point:

```bash
bash test.sh
```

If the script prints `[TEST OK] all three sub-drivers finished` and exits with code 0, the install is complete. See [README.md](README.md) §"Quick Run" for details.

---


