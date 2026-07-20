# STATUS

> This document declares which badges the authors are applying for and why each badge is satisfied.
>
> **Distribution channel.** The artifact is published exclusively via **Zenodo**; no GitHub mirror is advertised to reviewers. The Zenodo record is the only canonical, immutable copy.

---

## 1. Badges Applied For

| Badge | Applied? |
|-------|----------|
| **Artifacts Available**     |  Yes |
| **Artifacts Evaluated – Functional**  |  Yes |
| Artifacts Evaluated – Reusable |  No |
| Results Reproduced          |  No |

---

## 2. Artifacts Available

### Official criterion
> The artifacts associated with the paper have been made permanently available for retrieval.

### How this artifact satisfies the criterion

| Requirement                                          | Status | Evidence |
|------------------------------------------------------|--------|----------|
| Public, immutable archival repository (e.g., Zenodo) | [10.5281/zenodo.21452170](https://doi.org/10.5281/zenodo.21452170) | The authors' working directory is the staging copy; the Zenodo record is the canonical archival copy and the only channel advertised to reviewers. |
| Long-term availability                               | ✅      | Zenodo uses CERN's archival infrastructure; guarantees ≥ 20-year retention. |
| Immutability / versioning                            | ✅      | This repo is pinned to tag `aggrekv-v1.0-artifact`. The Zenodo record will reference this exact tag. |
| Permanent identifier (DOI)                           | [10.5281/zenodo.21452170](https://doi.org/10.5281/zenodo.21452170) | Reserved on Zenodo; the same DOI will be retained on the published record. |
| Open license that allows comparison and extension   | ✅      | See [LICENSE](LICENSE) — BSD 3-Clause. |
| README referencing the paper                         | ✅      | See [README.md](README.md) §"Paper". |

### Upload procedure (recommended)

1. **Reserve a DOI** by uploading a minimal placeholder archive (e.g. this README + a stub tarball) and clicking *Reserve DOI*. Zenodo immediately issues a DOI of the form `10.5281/zenodo.XXXXXXX`.
2. **Fill in this section**: replace the `<TODO: …>` placeholders below with the real DOI, record URL, and upload date. Also fill in [README.md](README.md) §2 (Accepted PDF) and §9 (Citation) with the same DOI.
3. **Publish the real version** by uploading the full artifact (`aggrekv-artifact.tar.gz`, ≈6.2 GB on disk) to the same record and clicking *Publish*. The DOI from step 1 is preserved on the published record.

> Do not delete a published Zenodo record — instead upload a *new version* (Zenodo's "New version" button). New versions receive a DOI suffix (e.g. `.v2`) but the canonical DOI of the original record remains valid.


### Accepted Paper PDF (bundled)

The accepted version of the paper is bundled at `paper/AggreKV_major.pdf`
so reviewers can read it without leaving the repository.

------

## 3. Artifacts Evaluated – Functional

### Official criterion
> The artifacts are documented, consistent, complete, exercisable, and include appropriate evidence of verification and validation.

### How this artifact satisfies the criterion

| Requirement                                              | Status | Evidence (file in this repo) |
|----------------------------------------------------------|--------|------------------------------|
| **Documented**: README describes contents, requirements, install, and a step-by-step procedure | ✅ | [README.md](README.md), [REQUIREMENTS.md](REQUIREMENTS.md), [INSTALL.md](INSTALL.md) |
| **Consistent**: terminology, file layout, and naming are coherent across docs and code | ✅ | Single top-level `test.sh` orchestrates all three baselines uniformly via `MODE=smoke|full`. |
| **Complete**: includes all code and data necessary to exercise the artifact | ✅ | All three sub-modules (`hash_kvssd`, `lsmtree_kvssd`, `block_ssd`) ship full source + bundled blktrace inputs (`block_ssd/blktrace/`). |
| **Exercisable**: scripts can be executed to completion and produce well-formed output | ✅ | `bash test.sh` exits 0 when all three sub-drivers (in `hash_kvssd`, `lsmtree_kvssd`, `block_ssd`) complete successfully; non-zero exit indicates the failing sub-driver. |
| **Evidence of verification**: each script documents its expected output path, and `test.sh` prints a clear pass/fail marker | ✅ | [README.md](README.md) §7 enumerates the summary file each sub-driver produces; a successful run is `bash test.sh` exiting 0 together with non-empty existence of all three summary files in their respective sub-module directories. |
| **Allowed license for review**                           | ✅      | [LICENSE](LICENSE) — BSD 3-Clause explicitly permits evaluation, comparison, and extension. |

> **Note on "evidence of verification"**: per the official Functional criteria, verification means the artifact can be **exercised and produces well-formed output**, NOT that the output matches the paper's numbers. Numerical alignment with the paper is the separate **Reproduced** badge (which we are not applying for). See [README.md](README.md) §"Sanity-Check Outputs" for what counts as a successful run.

---

## 4. Risks and Limitations

> Honest disclosure to reviewers.

- **Single-machine run**: all numbers were captured on one machine; cross-machine variance is `<TODO: report σ across N runs>`.
- **blktrace dataset size**: `block_ssd/blktrace/` is 6.2 GB; it is bundled inside the single Zenodo archive (`aggrekv-artifact.tar.gz`). Reviewers who only want to exercise the AggreKV and LSM-tree baselines may delete `block_ssd/blktrace/` after extraction — the Block-SSD baseline will skip with a clear message.
- **YCSB workloads are bundled**: workloads are generated deterministically from the bundled seeds inside the test drivers — no external RocksDB installation is required.
- **The AggreKV hot/cold data-segregation flags (`-DHOT_CMT -DADAPTIVE_MEM -DDATA_SEGREGATION`)** must remain enabled in the published build; disabling any one of them regresses AggreKV into a plain hash-based KV-SSD baseline.

`<TODO: add any additional limitations observed after the final reproduction run.>`

---

## 5. License

[BSD 3-Clause](LICENSE). Reviewers are explicitly granted the rights to: run the artifact for evaluation, modify for personal evaluation, and compare against other artifacts.
