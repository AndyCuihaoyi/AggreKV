#define DVALUE
// #define DEBUG_FTL

#define MAX_INF_REQS (65536)

#define MAX_HASH_COLLISION (1024)

#define KVSSD
#define MAXKEYSIZE 128 // in bytes - 支持长键场景
#define MAX_CACHED_KEY_LEN 32 // key cache 截断长度，超过此长度的键截断存储
#define MAX_WRITE_BUF 256

#define K 1024
#define M (1024 * K)
#define G (1024 * M)
#define PIECE 1024
#define GRAINED_UNIT PIECE // 1024
#define PAGESIZE 4096
#define GRAIN_PER_PAGE (PAGESIZE / GRAINED_UNIT) // 4
#define NPCINPAGE (PAGESIZE / PIECE)             // 4

/* 临时调小：LPA 空间从 1TB 缩到 64GB（原 (1<<26) / 16 = (1<<22) = 4M pages =
 * 64GB LPA） 配 ssd.c blks_per_pl=74 → 物理 74*8*4*2048*8*512 = 18GB 物理容量
 * 物理 18GB ≈ 18GB，FTL _NOS=73，能更快触发 GC */
#define _NOP_NO_OP ((1ULL << 26))          // 256GB LPA space (4M pages)
#define _NOP ((_NOP_NO_OP * 115ULL) / 100) // 15% OP for logical space
#define _PPS (1ULL << 16)                  // 256MB (匹配 pgs_per_line=65536)
#define _NOS (_NOP / _PPS)

#define STORE_KEY_FP
#define UPDATE_DATA_CHECK

/* Ghost Cache + chunk-based adaptive memory (requires -DHOT_CMT) */
// #define ADAPTIVE_MEM
#if defined(ADAPTIVE_MEM) && !defined(HOT_CMT)
#error ADAPTIVE_MEM requires HOT_CMT
#endif

/* Initial DRAM partition ratios (overridden at runtime via cache_env) */
#define DEFAULT_HOT_CMT_FRAC (0.05)
#define DEFAULT_FULL_KEY_FRAC (0.02)

#ifdef ADAPTIVE_MEM
/* chunk_tpages = total_cache_tpages * ADAPTIVE_CHUNK_FRAC_PCT / 100 */
#define ADAPTIVE_CHUNK_FRAC_PCT (1)
/* Expand 后 Hot 占 Cold+Hot pool 上限（%） */
#define ADAPTIVE_HOT_MAX_POOL_PCT (20)
/* Key Cache 占 total_cache_tpages 上限（%）；10% × total ≈ 10% × 逻辑池 / 1024
 * ≈ 内存 × 10% */
#define ADAPTIVE_KEY_MAX_POOL_PCT (10)
/* N_hot=(chunk×EPP)/DIV；DIV 越小 N_hot 越大，Expand 越少 */
#define ADAPTIVE_REBALANCE_THRESH_DIV (256)
/* 后台 Hot rehash：每 tick 扫描/插入预算；rebalance 后 drain 轮数 */
#define HOT_REHASH_SCAN_BUDGET (32768)
#define HOT_REHASH_INSERT_BUDGET (8192)
#define HOT_REHASH_DRAIN_ROUNDS (48)
#endif

// #define CMT_USE_NUMA
