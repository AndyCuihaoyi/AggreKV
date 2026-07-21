# STATUS

This document declares which badges the authors are applying for and why each badge is satisfied.

The canonical source tree lives on GitHub. A persistent, DOI-indexed archive is published on Zenodo via Zenodo's GitHub integration; that DOI is the link submitted on the CASES HotCRP form and the one reviewers should cite.

---

## 1. Badges Applied For

| Badge | Applied? |
|-------|----------|
| **Artifacts Available** | Yes |
| **Artifacts Evaluated – Functional** | Yes |
| Artifacts Evaluated – Reusable | No |
| Results Validated – Reproduced | No |

---

## 2. Artifacts Available

### Official criterion
> Author-created artifacts relevant to the associated paper have been placed on a publically accessible archival repository. A DOI or link to this repository along with a unique identifier for the object is provided.

### How this artifact satisfies the criterion

| Requirement | Status |
|-------------|--------|
| Public, immutable archival repository | Satisfied |
| Long-term availability | Satisfied |
| Immutability / versioning | Satisfied |
| Permanent identifier (DOI) | Pending (filled after the GitHub `v1.0.0` Release triggers Zenodo integration) |
| Open license that allows comparison and extension | Satisfied |
| README referencing the paper | Satisfied |

The Zenodo DOI and record URL will be filled in once the record is published. They are recorded in the CASES HotCRP submission form so reviewers can access the artifact immediately; the same DOI appears on the front page of the published paper.

### Recommended Zenodo workflow

1. Connect the GitHub repository to Zenodo and click *Reserve DOI* on a stub release. Zenodo immediately issues a DOI of the form `10.5281/zenodo.XXXXXXX`.
2. Record that DOI in the CASES HotCRP submission form.
3. Publish the full release on Zenodo (or push the `aggrekv-v1.0-artifact` tag to GitHub so Zenodo creates a versioned record). The DOI from step 1 is preserved on the published record.

Do not delete a published Zenodo record — instead upload a new version (Zenodo's "New version" button). New versions receive a DOI suffix (e.g. `.v2`) but the canonical DOI of the original record remains valid.

---

### Related Zenodo records

This artifact is associated with **two** Zenodo records, each carrying its own DOI:

| Object         | Record URL                                          | DOI                       |
|----------------|-----------------------------------------------------|---------------------------|
| Source code (this GitHub repository) | https://zenodo.org/records/<TODO: source record id>     | `<TODO: source DOI>`      |
| Blktrace dataset | <https://zenodo.org/records/21455311>             | `10.5281/zenodo.21455311` |

The source-code record is created automatically by Zenodo's GitHub-integration workflow when the `v1.0.0` tag is pushed to GitHub. The blktrace-dataset record was published manually with a separate DOI because the 6.2 GB dataset exceeds Zenodo's per-record download-archive size limit. Reviewers cite both DOIs in any derivative work.


## 3. Artifacts Evaluated – Functional

### Official criterion
> The artifacts associated with the research are found to be documented, consistent, complete, exercisable, and include appropriate evidence of verification and validation. The reviewer must be able to run some examples related to the paper, but it is not necessary that it replicates the very same results as in the paper.

### How this artifact satisfies the criterion

| Requirement | Status | Evidence file in this repo |
|-------------|--------|----------------------------|
| **Documented**: README describes contents, requirements, install, and a step-by-step procedure | Satisfied | `README.md`, `REQUIREMENTS.md`, `INSTALL.md` |
| **Consistent**: terminology, file layout, and naming are coherent across docs and code | Satisfied | Top-level `test.sh`; uniform sub-module naming (`Makefile` + `run_*.sh` + per-module results dir); doc filenames match the CASES-prescribed list |
| **Complete**: includes all code and data necessary to exercise the artifact | Satisfied | `hash_kvssd/`, `lsmtree_kvssd/`, `block_ssd/` source trees; the 6.2 GB blktrace dataset is fetched separately per `INSTALL.md` §3.1 |
| **Exercisable**: scripts can be executed to completion and produce well-formed output | Satisfied | `test.sh` (root-level thin orchestrator); the three sub-drivers each in their own module |
| **Evidence of verification & validation**: each sub-driver's expected output path is documented, and `test.sh` prints a clear pass/fail marker | Satisfied | `hash_kvssd/hash_kvssd_results/summary/summary.txt`; `lsmtree_kvssd/lsmtree_test_results/summary.txt`; `block_ssd/blktrace_test_results/test_*.log` and `block_ssd/blktrace_summary/summary_<timestamp>.txt` (full list in `README.md` §"Outputs and How to Interpret Them"); success marker is `bash test.sh` exiting 0 with `[TEST OK] all three sub-drivers finished` |

Under the Functional badge, verification means the artifact can be **exercised and produces well-formed output**. It does **not** require that the output match the paper's numbers. Numerical alignment with the paper is the separate Reproduced badge, which we are not applying for. See `README.md` §"Outputs and How to Interpret Them" for what counts as a successful run.

---

## 4. Risks and Limitations

- **Single-machine run.** All numbers in the paper were captured on one machine; cross-machine variance is not characterized.
- **blktrace dataset size.** `block_ssd/blktrace/` is 6.2 GB; it is not in the GitHub repo and is fetched separately from a second Zenodo dataset record per `INSTALL.md` §3.1. Reviewers who only want to exercise the AggreKV and LSM-tree baselines may skip that step — `run_blktrace_tests.sh` then exits 0 with a clear skip message.
- **YCSB workloads are generated deterministically** from the bundled `mt19937` seeds inside each test driver — no external RocksDB installation is required.
- **The AggreKV hot/cold data-segregation flags (`-DHOT_CMT -DADAPTIVE_MEM -DDATA_SEGREGATION`)** must remain enabled in the published build. They are passed to gcc via the `PROFILE_*_CFLAGS` variables in `hash_kvssd/run_all_hashkvssd_tests.sh`; disabling any one of them regresses AggreKV into a plain hash-based KV-SSD baseline. See `REQUIREMENTS.md` §"Disallowed Modifications".

---

## 5. License

BSD 3-Clause. See `LICENSE`. The license terms permit running, modifying, and comparing the artifact for evaluation.
