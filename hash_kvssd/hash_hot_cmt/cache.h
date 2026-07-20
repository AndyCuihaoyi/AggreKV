/*
 * Header for Cache module
 */

#ifndef __CACHE_H__
#define __CACHE_H__

#include "dftl_types.h"
#include "../tools/lru_list.h"
#include "../tools/skiplist.h"

/* Structures */
struct cache_env
{
	uint64_t nr_tpages_optimal_caching;
	uint64_t nr_valid_tpages;
	uint64_t nr_valid_tentries;

	uint64_t max_cached_tpages;
	uint64_t max_cached_tentries;
#ifdef HOT_CMT
	double hot_cmt_frac;
	double full_key_frac;
	uint64_t max_cached_hot_tpages;
	uint64_t max_cached_hot_entries;
	/* Runtime free key bytes (VERIFY_CACHE); decremented by key_len on cache-in, added on evict. */
	uint64_t max_cached_full_key;
	/* Fixed key-cache DRAM budget in bytes (DEFAULT_FULL_KEY_FRAC init); partition ratio only. */
	uint64_t max_cached_full_key_budget;
#ifdef ADAPTIVE_MEM
	uint64_t total_cache_tpages;
	/* Hot 活跃页上限（≤ total_cache_tpages × ADAPTIVE_HOT_MAX_POOL_PCT / 100） */
	uint64_t hot_tpages_cap;
	/* Cold / Hot 各自至少保留的页数，防止被 chunk 搬空（= chunk_tpages） */
	uint64_t min_cached_tpages;
	uint64_t min_cached_hot_tpages;
	/*
	 * 单次 expand/shrink 移动的 mapping 页数：
	 *   chunk_tpages = total_cache_tpages * ADAPTIVE_CHUNK_FRAC_PCT / 100
	 */
	uint64_t chunk_tpages;
	/* rebalance 触发门槛（Hot CMT 周期换出计数，见 adaptive_try_rebalance） */
	uint64_t n_hot;
	/*
	 * Key Cache 独立上限和扩容步长（单位：bytes）：
	 * - key_entries_cap = total_cache_tpages * ADAPTIVE_KEY_MAX_POOL_PCT / 100 * EPP * MAXKEYSIZE
	 * - key_chunk_entries = key_entries_cap * ADAPTIVE_CHUNK_FRAC_PCT / 100
	 */
	uint64_t key_entries_cap;
	uint64_t key_chunk_entries;
	/* Ghost ring capacities (4B IDs only; see adaptive_mem.c) */
	uint64_t ghost_cold_cap;
	uint64_t ghost_key_cap;
	/* Hot+Key stolen DRAM as % of total cache budget (for logging) */
	uint64_t ghost_cold_frac_pct;
#endif
#endif
	uint64_t max_cache_entry;

	/* add attributes here */
	algorithm *palgo;
};

struct cache_member
{
	struct cmt_struct **cmt;
	struct pt_struct **mem_table;
#ifdef HOT_CMT
	struct cmt_struct **hot_cmt;
	struct hot_pt_struct **hot_mem_table;
#endif
	LRU *lru;

	int nr_cached_tpages;
	int nr_cached_tentries;
#ifdef HOT_CMT
	int nr_cached_hot_tpages;
	int nr_cached_hot_tentries;
#endif
#ifdef PREFILL_CACHE
	Queue prefill_q;
#endif

	/* add attributes here */
	volatile int nr_tpages_read_done;
	volatile int nr_valid_read_done;
#ifdef ADAPTIVE_MEM
	uint64_t nr_ghost_cold;
	uint64_t nr_ghost_key;
#endif
};

struct cache_stat
{
	/* cache performance */
	uint64_t cache_hit;
	uint64_t cache_miss;
	uint64_t clean_evict;
	uint64_t dirty_evict;
	uint64_t blocked_miss;

	/* add attributes here */
	uint64_t cache_miss_by_collision;
	uint64_t cache_hit_by_collision;
	uint64_t cache_load;

#ifdef HOT_CMT
	uint64_t hot_cmt_hit;
	/* Hot 有效 grain 数：promote_hot 填空 ++，adaptive_clear_hot_page 清空 -- */
	uint64_t hot_valid_entries;
	uint64_t hot_rewrite_entries;
	uint64_t hot_promote_blocked_cnt; /* 累计：探测链全满且 victim 热度 > incoming，替换失败 */
	uint64_t up_grain_cnt;     /* 累计：promote_hot 尝试晋升的 grain 次数（含覆盖/更新） */
	uint64_t up_grain_new_cnt; /* 累计：实际填入空槽的新 grain 次数（与 hot_valid++ 同步） */
	uint64_t up_hit_cnt;
	uint64_t up_page_cnt;
	/* promote 时 Cold cnt_map 分布（非 Hot PTE state） */
	uint32_t *grain_heat_distribute;
	/* Hot 命中并 raise 后的 PTE state 直方图（0-5，与 STATE_NUM 一致） */
	uint64_t hot_pte_state_on_hit[6];
	/* Hot 表各档位驻留 grain 数（命中/晋升/decay/挤占/rehash 增量维护，不扫描） */
	uint64_t hot_pte_state_live[6];
	/* 本 batch 晋升页快照：页内全部有效 grain 的 cnt_map 档位（含 0-1，batch 后清零） */
	uint64_t cold_promote_page_grain_period[6];
	/* 本 batch 实际 promote 的 grain 档位（cnt>=COLD_CNT_PROMOTE_THRESH，batch 后清零） */
	uint64_t cold_promote_grain_period[6];
	uint64_t cold_promote_pages_period;
	uint64_t cold_promote_page_heat_sum_period;
#endif
#ifdef ADAPTIVE_MEM
	/*
	 * Period counters (cleared after each rebalance); access via adaptive_stat_* only.
	 * ghost_hit_*: Ghost Cache hits this rebalance period (Phase 1).
	 * hot/cold_evict_count: promote 挤占 victim 3-5→Hot_Evict，0-2→Cold_Evict；
	 *   distinct from hot_rewrite_entries (lifetime, show_stats).
	 * expand/shrink_count: cumulative rebalance events.
	 */
	uint64_t ghost_hit_cold;
	uint64_t ghost_hit_key;
	uint64_t hot_evict_count;
	uint64_t cold_evict_count;
	uint64_t expand_count;
	uint64_t shrink_count;
	uint64_t ghost_key_state_live[6]; /* Ghost Key 各档位（0-5）驻留数 */
#endif
};

typedef struct demand_cache
{
	int (*create)(struct demand_cache *);
	int (*destroy)(struct demand_cache *);

	int (*load)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
	int (*list_up)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
	int (*wait_if_flying)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);

	int (*touch)(struct demand_cache *self, lpa_t lpa);
	int (*update)(struct demand_cache *self, lpa_t lpa, struct pt_struct pte);

	struct pt_struct (*get_pte)(struct demand_cache *self, lpa_t lpa);
	struct cmt_struct *(*get_cmt)(struct demand_cache *self, lpa_t lpa);

	bool (*is_hit)(struct demand_cache *self, lpa_t lpa);
	bool (*is_full)(struct demand_cache *self);
#ifdef HOT_CMT
	int (*promote_hot)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim);
	bool (*hot_is_hit)(struct demand_cache *self, lpa_t lpa, struct hot_pt_struct **hot_pte);
	int (*hot_cmt_reset)(struct demand_cache *self);
#endif
	struct cache_env env;
	struct cache_member member;
	struct cache_stat stat;
} demand_cache;

#ifdef ADAPTIVE_MEM
#if defined(HOT_CMT)
#include "demand.h"

typedef enum {
	ADAPTIVE_EV_SUMMARY = 0,
	ADAPTIVE_EV_INIT,
	ADAPTIVE_EV_EXPAND,
	ADAPTIVE_EV_SHRINK,
} adaptive_event_t;

void adaptive_show_stats(const demand_cache *dc);
void adaptive_show_stats_event(const demand_cache *dc, adaptive_event_t ev,
			       const char *detail);
void adaptive_mem_init(void);
void adaptive_stats_bootstrap(demand_cache *dc);
void adaptive_reset_stats(demand_cache *dc);
void adaptive_prepare_for_benchmark(demand_cache *dc);
void adaptive_stat_inc_hot_evict(demand_cache *dc);
void adaptive_stat_inc_cold_evict(demand_cache *dc);
void adaptive_ghost_destroy(void);
void adaptive_ghost_cold_mark(demand_cache *dc, struct cmt_struct *cmt);
void adaptive_ghost_cold_clear(demand_cache *dc, struct cmt_struct *cmt);
void adaptive_ghost_cold_on_miss(demand_cache *dc, struct cmt_struct *cmt,
				 bool is_update);
void adaptive_ghost_key_mark(demand_cache *dc, struct hot_pt_struct *slot,
			    uint64_t hot_page, uint64_t hot_grain);
bool adaptive_ghost_key_on_flash_verify(demand_cache *dc, struct hot_pt_struct *hot_pte,
				      fp_t key_fp);
void adaptive_try_rebalance(demand_cache *dc);
/* Hot CMT 专用锁 + 后台 rehash（rehash 期间关键路径不得访问 Hot CMT） */
void hot_cmt_init(void);
void hot_cmt_destroy(void);
void hot_cmt_table_lock(void);
void hot_cmt_table_unlock(void);
bool hot_cmt_accessible(void);
int hot_rehash_phase(void);
const char *hot_rehash_phase_name(int phase);
uint64_t hot_cmt_fill_permille(const demand_cache *dc);
uint64_t hot_cmt_warm_fill_permille(const demand_cache *dc);
uint64_t hot_cmt_report_valid_entries(const demand_cache *dc);
void hot_rehash_begin(demand_cache *dc);
void hot_rehash_tick(demand_cache *dc);
void hot_rehash_drain(demand_cache *dc, uint32_t rounds);
void hot_rehash_drain_all(demand_cache *dc);
/* One-line period counters + expand/shrink thresholds (for per-batch test logs) */
void adaptive_log_rebalance_period(const demand_cache *dc, const char *tag);
#endif /* HOT_CMT */
#endif /* ADAPTIVE_MEM */

#ifdef HOT_CMT
void cmt_heat_log_batch_period(demand_cache *dc, const char *tag);
void hot_cmt_hist_on_clear_slot(demand_cache *dc, const struct hot_pt_struct *slot);
void dftl_cache_cold_grain_access(demand_cache *self, lpa_t lpa);
#endif

#endif /* __CACHE_H__ */
