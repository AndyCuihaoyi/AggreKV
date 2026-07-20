#include "dftl_cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "bm.h"
#include "demand.h"
#include "request.h"
#include "../lower/lower.h"
#include "dftl_pg.h"
#include "glib-2.0/glib.h"
#include "write_buffer.h"
#include "dftl_settings.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
#ifdef CMT_USE_NUMA
#include <numa.h>
#endif // CMT_USE_NUMA
#ifdef HOT_CMT
#define MAX_PROBE (50)
#define HOT_CMT_PROMOTE_THRESHOLD (70) /* 页内热 grain 占比阈值（当前逻辑已注释） */
#endif
extern block_mgr_t bm;
extern block_mgr_t *pbm;
#ifdef HOT_CMT
int dftl_cache_promote_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim);
bool dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, h_pte_t **hot_pte);

static inline void hot_hist_dec(demand_cache *self, uint8_t state)
{
	if (self != NULL && state < STATE_NUM && self->stat.hot_pte_state_live[state] > 0)
		self->stat.hot_pte_state_live[state]--;
}

static inline void hot_hist_inc(demand_cache *self, uint8_t state)
{
	if (self != NULL && state < STATE_NUM)
		self->stat.hot_pte_state_live[state]++;
}

void hot_cmt_hist_on_clear_slot(demand_cache *dc, const h_pte_t *slot)
{
	if (dc == NULL || slot == NULL || slot->lpa == UINT32_MAX)
		return;
	hot_hist_dec(dc, slot->state);
}

static inline void hot_hist_move(demand_cache *self, h_pte_t *entry, uint8_t new_state)
{
	if (entry == NULL || entry->lpa == UINT32_MAX || new_state >= STATE_NUM)
		return;
	hot_hist_dec(self, entry->state);
	entry->state = new_state;
	hot_hist_inc(self, entry->state);
}

static inline void hot_state_raise(demand_cache *self, h_pte_t *entry)
{
	if (entry->lpa == UINT32_MAX)
		return;
	if (entry->state < HOT_STATE_MAX)
		hot_hist_move(self, entry, entry->state + 1);
	if (self != NULL && entry->state < STATE_NUM)
		self->stat.hot_pte_state_on_hit[entry->state]++;
}

static inline uint8_t hot_promote_entry_state(uint8_t cold_cnt)
{
	if (cold_cnt > HOT_STATE_MAX)
		return HOT_STATE_MAX;
	return cold_cnt;
}

/* 晋升写入：沿用 Cold cnt_map 档位，不再固定 DEFAULT_STATE */
static inline void hot_hist_place_promoted(demand_cache *self, h_pte_t *entry, uint8_t cold_cnt)
{
	uint8_t heat = hot_promote_entry_state(cold_cnt);

	entry->state = heat;
	hot_hist_inc(self, heat);
}

static inline void hot_hist_sync_promoted(demand_cache *self, h_pte_t *entry, uint8_t cold_cnt)
{
	uint8_t heat = hot_promote_entry_state(cold_cnt);

	if (entry->state != heat)
		hot_hist_move(self, entry, heat);
}

static inline void hot_probe_advance(int *d_idx, int *p_idx, int probe_step, int max_tpages)
{
	int p = *p_idx + probe_step * probe_step;

	*d_idx = (*d_idx + p / EPP) % max_tpages;
	*p_idx = p % EPP;
}

/*
 * Promote 换出压力（rebalance）：victim.state 3-5→Hot_Evict，0-2→Cold_Evict；
 * 挡回（cold_cnt < victim.state）时 victim>=3 亦计 Hot_Evict。
 */
static void hot_promote_adaptive_evict_stat(demand_cache *self, const h_pte_t *victim,
					    uint8_t incoming_cnt)
{
#ifdef ADAPTIVE_MEM
	if (promote_displace_counts_hot_evict(victim, incoming_cnt))
		adaptive_stat_inc_hot_evict(self);
	else
		adaptive_stat_inc_cold_evict(self);
#else
	(void)self;
	(void)victim;
	(void)incoming_cnt;
#endif
}

static void hot_promote_adaptive_block_stat(demand_cache *self, const h_pte_t *victim)
{
#ifdef ADAPTIVE_MEM
	if (hot_pte_counts_hot_evict(victim))
		adaptive_stat_inc_hot_evict(self);
#else
	(void)self;
	(void)victim;
#endif
}

/*
 * Promote：优先空槽，否则替换探测链上 state 最小项（同档取探测序靠后者）。
 * 挤占：cold_cnt >= victim.state 即替换（含同档相等，Cold 侧更新更近）。
 * 写入 state = cold_cnt；不做探测链 decay（热度仅由 cnt_map / 命中 raise 维护）。
 */
static bool hot_promote_place_grain(demand_cache *self, lpa_t lpa, uint32_t ppa, fp_t key_fp,
				   uint8_t cold_cnt)
{
	lpa_t new_lpa = lpa % self->env.max_cached_hot_entries;
	int max_tpages = (int)self->env.max_cached_hot_tpages;
	int d_idx = (int)(new_lpa / EPP);
	int p_idx = (int)(new_lpa % EPP);
	int empty_d = -1;
	int empty_p = -1;
	int victim_d = -1;
	int victim_p = -1;
	uint8_t victim_state = HOT_STATE_MAX + 1;
	h_pte_t *slot;
	int probe;

	for (probe = 0; probe < MAX_PROBE; probe++)
	{
		slot = &self->member.hot_mem_table[d_idx][p_idx];
		if (slot->lpa == lpa)
		{
			slot->ppa = ppa;
			slot->key_fp = key_fp;
			hot_hist_sync_promoted(self, slot, cold_cnt);
#ifdef ADAPTIVE_MEM
			slot->key_ghost_mark = false;
#endif
			/* 同一 LPA：键未变，保留 real_key 以拦截后续 data check */
			return true;
		}
		if (slot->lpa == UINT32_MAX)
		{
			if (empty_d < 0)
			{
				empty_d = d_idx;
				empty_p = p_idx;
			}
		}
		else
		{
			if (slot->state < victim_state)
			{
				victim_state = slot->state;
				victim_d = d_idx;
				victim_p = p_idx;
			}
			else if (slot->state == victim_state)
			{
				/* 同档：后者覆盖前者，便于 cold_cnt>=state 时用新晋升条目替换 */
				victim_d = d_idx;
				victim_p = p_idx;
			}
		}
		if (probe + 1 >= MAX_PROBE)
			break;
		hot_probe_advance(&d_idx, &p_idx, probe + 1, max_tpages);
	}

	if (empty_d >= 0)
	{
		d_idx = empty_d;
		p_idx = empty_p;
		self->stat.hot_valid_entries++;
		self->stat.up_grain_new_cnt++;
	}
	else if (victim_d >= 0)
	{
		slot = &self->member.hot_mem_table[victim_d][victim_p];
		/* cold_cnt >= victim.state 方可挤占（同档相等亦替换） */
		if (cold_cnt < slot->state)
		{
			hot_promote_adaptive_block_stat(self, slot);
			self->stat.hot_promote_blocked_cnt++;
			return false;
		}
		d_idx = victim_d;
		p_idx = victim_p;
		self->stat.hot_rewrite_entries++;
		hot_promote_adaptive_evict_stat(self, slot, cold_cnt);
		if (slot->lpa != UINT32_MAX)
			hot_hist_dec(self, slot->state);
	}
	else
		return false;

	slot = &self->member.hot_mem_table[d_idx][p_idx];
#ifdef VERIFY_CACHE
	if (slot->real_key.len != 0)
	{
#ifdef ADAPTIVE_MEM
		adaptive_ghost_key_mark(self, slot, d_idx, p_idx);
#endif
		self->env.max_cached_full_key += slot->real_key.len;
	}
	slot->real_key.len = 0;
	slot->real_key.key[0] = 0;
#endif
#ifdef ADAPTIVE_MEM
	slot->key_ghost_mark = false;
#endif
	slot->lpa = lpa;
	slot->ppa = ppa;
	slot->key_fp = key_fp;
	hot_hist_place_promoted(self, slot, cold_cnt);
	return true;
}

void cmt_heat_log_batch_period(demand_cache *dc, const char *tag)
{
	if (dc == NULL)
		return;

	ftl_log("  [%s cmt-heat] Hot live(state0-5): %" PRIu64 " %" PRIu64 " %" PRIu64
		" %" PRIu64 " %" PRIu64 " %" PRIu64 " hot_valid=%" PRIu64
		" blocked=%" PRIu64 "\n",
		tag ? tag : "batch", dc->stat.hot_pte_state_live[0],
		dc->stat.hot_pte_state_live[1], dc->stat.hot_pte_state_live[2],
		dc->stat.hot_pte_state_live[3], dc->stat.hot_pte_state_live[4],
		dc->stat.hot_pte_state_live[5], dc->stat.hot_valid_entries,
		dc->stat.hot_promote_blocked_cnt);

	if (dc->stat.cold_promote_pages_period > 0)
	{
		ftl_log("  [%s cmt-heat] Cold promote pages=%" PRIu64
			" cnt[0-5]: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
			" %" PRIu64 " %" PRIu64 "\n",
			tag ? tag : "batch", dc->stat.cold_promote_pages_period,
			dc->stat.cold_promote_page_grain_period[0],
			dc->stat.cold_promote_page_grain_period[1],
			dc->stat.cold_promote_page_grain_period[2],
			dc->stat.cold_promote_page_grain_period[3],
			dc->stat.cold_promote_page_grain_period[4],
			dc->stat.cold_promote_page_grain_period[5]);
	}
	else
	{
		ftl_log("  [%s cmt-heat] Cold promote: (no grains this batch)\n",
			tag ? tag : "batch");
	}

	memset(dc->stat.cold_promote_page_grain_period, 0,
	       sizeof(dc->stat.cold_promote_page_grain_period));
	memset(dc->stat.cold_promote_grain_period, 0,
	       sizeof(dc->stat.cold_promote_grain_period));
	dc->stat.cold_promote_pages_period = 0;
	dc->stat.cold_promote_page_heat_sum_period = 0;
}
#endif /* HOT_CMT */

demand_cache d_cache = {
	.create = dftl_cache_create,
	.destroy = dftl_cache_destroy,
	.load = dftl_cache_load,
	.list_up = dftl_cache_list_up,
	.wait_if_flying = dftl_cache_wait_if_flying,
	.touch = dftl_cache_touch,
	.update = dftl_cache_update,
	.get_pte = dftl_cache_get_pte,
	.get_cmt = dftl_cache_get_cmt,
	.is_hit = dftl_cache_is_hit,
	.is_full = dftl_cache_is_full,
#ifdef HOT_CMT
	.promote_hot = dftl_cache_promote_hot,
	.hot_is_hit = dftl_cache_hot_is_hit,
	.hot_cmt_reset = dftl_cache_hot_reset,
#endif
};

demand_cache *pd_cache = &d_cache;

static inline void ensure_cmt_queues(struct cmt_struct *cmt)
{
	if (!cmt->retry_q)
	{
		cmt->retry_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
	if (!cmt->wait_q)
	{
		cmt->wait_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
}

static void cache_env_init(struct cache_env *env)
{
	/* hash table cache */
	env->nr_tpages_optimal_caching = _NOP_NO_OP * 4 / PAGESIZE;
	// num of mapping page div hash factor(0.75)
	env->nr_valid_tpages = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // 0.75 is for hash table load factor
	env->nr_valid_tentries = env->nr_valid_tpages * EPP;
	env->max_cache_entry = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // number of tpages
#ifdef HOT_CMT
	env->hot_cmt_frac = DEFAULT_HOT_CMT_FRAC;
#ifdef VERIFY_CACHE
	env->full_key_frac = DEFAULT_FULL_KEY_FRAC;
#else
	env->full_key_frac = 0.0;
#endif
	env->max_cached_tpages =
	    ceil(_NOP_NO_OP / 1024 * (1 - env->hot_cmt_frac - env->full_key_frac));
	env->max_cached_hot_tpages = ceil(_NOP_NO_OP / 1024 * env->hot_cmt_frac);
	env->max_cached_full_key_budget =
	    ceil(_NOP_NO_OP / 1024 * env->full_key_frac) * PAGESIZE;
	env->max_cached_full_key = env->max_cached_full_key_budget;
	env->max_cached_hot_entries = env->max_cached_hot_tpages * EPP;
#ifdef ADAPTIVE_MEM
	env->total_cache_tpages = env->max_cached_tpages + env->max_cached_hot_tpages;
	env->hot_tpages_cap =
	    env->total_cache_tpages * ADAPTIVE_HOT_MAX_POOL_PCT / 100;
	env->chunk_tpages =
	    env->total_cache_tpages * ADAPTIVE_CHUNK_FRAC_PCT / 100;
	if (env->chunk_tpages == 0)
		env->chunk_tpages = 1;
	env->min_cached_tpages = env->chunk_tpages;
	env->min_cached_hot_tpages = env->chunk_tpages;
	env->n_hot = (env->chunk_tpages * EPP) / ADAPTIVE_REBALANCE_THRESH_DIV;
	if (env->n_hot == 0)
		env->n_hot = 1;
	/* Key Cache 上限：total_cache_tpages × 10% × EPP × MAXKEYSIZE（单位 bytes） */
	env->key_entries_cap =
	    env->total_cache_tpages * ADAPTIVE_KEY_MAX_POOL_PCT / 100 * EPP * MAXKEYSIZE;
	/* Key Cache 扩容步长：总内存的 1%（即 total_cache_tpages × 1% × EPP × MAXKEYSIZE） */
	env->key_chunk_entries =
	    env->total_cache_tpages * ADAPTIVE_CHUNK_FRAC_PCT / 1000 * EPP * MAXKEYSIZE;
	if (env->key_chunk_entries == 0)
		env->key_chunk_entries = 1;
#endif
#else
	env->max_cached_tpages = _NOP_NO_OP / 1024;
#endif
#ifdef BLOCK_SSD
	env->max_cached_tpages *= 2;
#endif
	env->max_cached_tentries = env->max_cached_tpages * EPP;
}

static void cache_member_init(struct cache_member *member)
{
	struct cmt_struct **cmt = g_malloc0(d_cache.env.nr_valid_tpages * sizeof(struct cmt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_tpages; i++)
	{
		cmt[i] = g_malloc0(sizeof(struct cmt_struct));
		cmt[i]->idx = i;
		cmt[i]->pt = NULL;
		cmt[i]->t_ppa = UINT32_MAX;
		cmt[i]->state = CLEAN;
		cmt[i]->is_flying = false;
		cmt[i]->lru_ptr = NULL;
		cmt[i]->is_cached = NULL;
		cmt[i]->cached_cnt = 0;
		cmt[i]->dirty_cnt = 0;
		cmt[i]->page_heat_sum = 0;
		cmt[i]->retry_q = NULL;
		cmt[i]->wait_q = NULL;
#ifdef ADAPTIVE_MEM
		cmt[i]->ghost_mark = false;
#endif
	}
	member->cmt = cmt;
	member->mem_table = g_malloc0(d_cache.env.nr_valid_tpages * sizeof(struct pt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		member->mem_table[i] = numa_alloc_onnode(EPP * sizeof(struct pt_struct), 1);
		memset(member->mem_table[i], 0, EPP * sizeof(struct pt_struct));
#else
		member->mem_table[i] = g_malloc0(EPP * sizeof(struct pt_struct));
		ftl_try_mlock(member->mem_table[i], EPP * sizeof(struct pt_struct),
			      "mem_table");
#endif // CMT_USE_NUMA
		for (int j = 0; j < EPP; j++)
		{
			member->mem_table[i][j].ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
			member->mem_table[i][j].key_fp = 0;
#endif
		}
	}
	lru_init(&(member->lru));
	member->nr_cached_tpages = 0;
	member->nr_cached_tentries = 0;
#ifdef HOT_CMT
	uint64_t hot_cap = d_cache.env.max_cached_hot_tpages;
#ifdef ADAPTIVE_MEM
	hot_cap = d_cache.env.hot_tpages_cap;
#endif
	struct cmt_struct **hot_cmt = g_malloc0(hot_cap * sizeof(struct cmt_struct *));
	for (uint64_t i = 0; i < hot_cap; i++)
	{
		hot_cmt[i] = g_malloc0(sizeof(struct cmt_struct));
		hot_cmt[i]->idx = i + d_cache.env.nr_valid_tpages;
		hot_cmt[i]->pt = NULL;
		hot_cmt[i]->t_ppa = UINT32_MAX;
		hot_cmt[i]->state = CLEAN;
		hot_cmt[i]->is_flying = false;
		hot_cmt[i]->lru_ptr = NULL;
		hot_cmt[i]->is_cached = NULL;
		hot_cmt[i]->cached_cnt = 0;
		hot_cmt[i]->dirty_cnt = 0;

		hot_cmt[i]->retry_q = NULL;
		hot_cmt[i]->wait_q = NULL;
	}
	member->hot_cmt = hot_cmt;
	member->hot_mem_table = g_malloc0(hot_cap * sizeof(struct hot_pt_struct *));
	for (uint64_t i = 0; i < hot_cap; i++)
	{

#ifdef CMT_USE_NUMA
		member->hot_mem_table[i] = numa_alloc_onnode(EPP * sizeof(struct pt_struct), 1);
		memset(member->hot_mem_table[i], 0, EPP * sizeof(struct pt_struct));
#else
		member->hot_mem_table[i] = g_malloc0(EPP * sizeof(struct hot_pt_struct));
		ftl_try_mlock(member->hot_mem_table[i],
			      EPP * sizeof(struct hot_pt_struct), "hot_mem_table");
#endif // CMT_USE_NUMA
		for (int j = 0; j < EPP; j++)
		{
			member->hot_mem_table[i][j].lpa = UINT32_MAX;
			member->hot_mem_table[i][j].ppa = UINT32_MAX;
			member->hot_mem_table[i][j].state = MIN_STATE;
#ifdef STORE_KEY_FP
			member->hot_mem_table[i][j].key_fp = 0;
#endif
#ifdef ADAPTIVE_MEM
			member->hot_mem_table[i][j].key_ghost_mark = false;
#endif
		}
	}
	member->nr_cached_hot_tpages = 0;
#ifdef ADAPTIVE_MEM
	member->nr_ghost_cold = 0;
	member->nr_ghost_key = 0;
#endif
	member->nr_cached_hot_tentries = 0;
#endif
#ifdef PREFILL_CACHE
	QInit(&(member->prefill_q), MAX_WRITE_BUF);
#endif
}

static void cache_stat_init(struct cache_stat *stat)
{
	stat->cache_hit = 0;
	stat->cache_miss = 0;
	stat->clean_evict = 0;
	stat->dirty_evict = 0;
	stat->blocked_miss = 0;
	stat->cache_miss_by_collision = 0;
	stat->cache_hit_by_collision = 0;
	stat->cache_load = 0;
#ifdef HOT_CMT
	stat->hot_cmt_hit = 0;
	stat->hot_valid_entries = 0;
	stat->hot_rewrite_entries = 0;
	stat->hot_promote_blocked_cnt = 0;
	stat->up_grain_cnt = 0;
	stat->up_grain_new_cnt = 0;
	stat->up_hit_cnt = 0;
	stat->up_page_cnt = 0;
	stat->grain_heat_distribute = g_malloc0(sizeof(uint32_t) * (1000));
	memset(stat->hot_pte_state_on_hit, 0, sizeof(stat->hot_pte_state_on_hit));
	memset(stat->hot_pte_state_live, 0, sizeof(stat->hot_pte_state_live));
	memset(stat->cold_promote_page_grain_period, 0,
	       sizeof(stat->cold_promote_page_grain_period));
	memset(stat->cold_promote_grain_period, 0, sizeof(stat->cold_promote_grain_period));
	stat->cold_promote_pages_period = 0;
	stat->cold_promote_page_heat_sum_period = 0;
#endif
}

int dftl_cache_create(demand_cache *d_cache)
{
	cache_env_init(&(d_cache->env));
	cache_member_init(&(d_cache->member));
	cache_stat_init(&(d_cache->stat));
#ifdef ADAPTIVE_MEM
	hot_cmt_init();
	adaptive_stats_bootstrap(d_cache);
#endif
	return 0;
}

static void cache_member_free(demand_cache *self, struct cache_member *member)
{
	for (int i = 0; i < self->env.nr_valid_tpages; i++)
	{
		if (member->cmt[i]->retry_q)
		{
			ring_free(member->cmt[i]->retry_q);
		}
		if (member->cmt[i]->wait_q)
		{
			ring_free(member->cmt[i]->wait_q);
		}
		if (member->cmt[i]->cnt_map)
		{
			g_free(member->cmt[i]->cnt_map);
			member->cmt[i]->cnt_map = NULL;
		}
		free(member->cmt[i]);
	}
	free(member->cmt);
	member->cmt = NULL;

	for (int i = 0; i < self->env.nr_valid_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		numa_free(member->mem_table[i], EPP * sizeof(struct pt_struct));
#else
		munlock(member->mem_table[i], EPP * sizeof(struct pt_struct));
		free(member->mem_table[i]);
#endif // CMT_USE_NUMA
	}
	free(member->mem_table);
	member->mem_table = NULL;
	lru_free(member->lru);
	member->lru = NULL;
#ifdef HOT_CMT
	{
		uint64_t hot_cap = self->env.max_cached_hot_tpages;
#ifdef ADAPTIVE_MEM
		hot_cap = self->env.hot_tpages_cap;
#endif
		for (uint64_t i = 0; i < hot_cap; i++)
			free(member->hot_cmt[i]);
		free(member->hot_cmt);
		member->hot_cmt = NULL;
		for (uint64_t i = 0; i < hot_cap; i++)
		{
#ifdef CMT_USE_NUMA
			numa_free(member->hot_mem_table[i], EPP * sizeof(struct pt_struct));
#else
			munlock(member->hot_mem_table[i], EPP * sizeof(struct hot_pt_struct));
			free(member->hot_mem_table[i]);
#endif // CMT_USE_NUMA
		}
		free(member->hot_mem_table);
		member->hot_mem_table = NULL;
	}
#endif
}

int dftl_cache_destroy(demand_cache *self)
{
#ifdef ADAPTIVE_MEM
	adaptive_ghost_destroy();
	hot_cmt_destroy();
#endif
	cache_member_free(self, &(self->member));
#ifdef HOT_CMT
	if (self->stat.grain_heat_distribute)
	{
		g_free(self->stat.grain_heat_distribute);
		self->stat.grain_heat_distribute = NULL;
	}
#endif
	return 0;
}

#ifdef HOT_CMT
#ifdef ADAPTIVE_MEM
typedef struct {
	h_pte_t pte;
} hot_rehash_entry_t;

#define HOT_REHASH_IDLE 0
#define HOT_REHASH_COLLECT 1
#define HOT_REHASH_INSERT 2
/* Rehash insert: keep probing for empty slot; do not use promote_hot eviction rules */
#define HOT_REHASH_MAX_PROBE (MAX_PROBE * 64)

static pthread_spinlock_t hot_cmt_lock;

static struct {
	volatile int phase;
	uint64_t n_pages;
	uint64_t scan_page;
	int scan_grain;
	hot_rehash_entry_t *saved;
	uint64_t n_saved;
	uint64_t saved_cap;
	uint64_t insert_idx;
	uint64_t placed;
	uint64_t ghosts;
	uint64_t insert_dropped;
	uint64_t collect_dropped_cold;
} hot_rehash_st;

static void hot_rehash_clear_slot(demand_cache *self, h_pte_t *slot)
{
	hot_cmt_hist_on_clear_slot(self, slot);
	slot->lpa = UINT32_MAX;
	slot->ppa = UINT32_MAX;
	slot->state = MIN_STATE;
#ifdef STORE_KEY_FP
	slot->key_fp = 0;
#endif
#ifdef ADAPTIVE_MEM
	if (slot->key_ghost_mark)
	{
		uint8_t state = slot->key_ghost_state < STATE_NUM
				  ? slot->key_ghost_state
				  : 0;
		self->stat.ghost_key_state_live[state]--;
		slot->key_ghost_mark = false;
		slot->key_ghost_state = 0;
		if (self->member.nr_ghost_key > 0)
			self->member.nr_ghost_key--;
	}
#endif
#ifdef VERIFY_CACHE
	slot->real_key.len = 0;
	slot->real_key.key[0] = 0;
#endif
}

static void hot_rehash_copy_pte(h_pte_t *dest, const h_pte_t *src)
{
	dest->lpa = src->lpa;
	dest->ppa = src->ppa;
	dest->state = src->state;
#ifdef STORE_KEY_FP
	dest->key_fp = src->key_fp;
#endif
	dest->key_ghost_mark = src->key_ghost_mark;
#ifdef VERIFY_CACHE
	dest->real_key = src->real_key;
#endif
}

static void hot_rehash_yield_key_budget_on_drop(demand_cache *self, const h_pte_t *slot)
{
#ifdef VERIFY_CACHE
	if (slot->real_key.len != 0 &&
	    self->env.max_cached_full_key < self->env.max_cached_full_key_budget)
		self->env.max_cached_full_key += slot->real_key.len;
#else
	(void)self;
	(void)slot;
#endif
}

void hot_cmt_init(void)
{
	pthread_spin_init(&hot_cmt_lock, PTHREAD_PROCESS_PRIVATE);
	memset(&hot_rehash_st, 0, sizeof(hot_rehash_st));
}

void hot_cmt_table_lock(void)
{
	pthread_spin_lock(&hot_cmt_lock);
}

void hot_cmt_table_unlock(void)
{
	pthread_spin_unlock(&hot_cmt_lock);
}

void hot_cmt_destroy(void)
{
	free(hot_rehash_st.saved);
	hot_rehash_st.saved = NULL;
	hot_rehash_st.saved_cap = 0;
	hot_rehash_st.phase = HOT_REHASH_IDLE;
}

bool hot_cmt_accessible(void)
{
	return hot_rehash_st.phase == HOT_REHASH_IDLE;
}

int hot_rehash_phase(void)
{
	return hot_rehash_st.phase;
}

const char *hot_rehash_phase_name(int phase)
{
	switch (phase)
	{
	case HOT_REHASH_COLLECT:
		return "COLLECT";
	case HOT_REHASH_INSERT:
		return "INSERT";
	default:
		return "IDLE";
	}
}

uint64_t hot_cmt_report_valid_entries(const demand_cache *dc)
{
	if (hot_rehash_st.phase == HOT_REHASH_IDLE)
		return dc->stat.hot_valid_entries;
	if (hot_rehash_st.phase == HOT_REHASH_INSERT)
		return hot_rehash_st.placed +
		       (hot_rehash_st.n_saved - hot_rehash_st.insert_idx);
	if (hot_rehash_st.phase == HOT_REHASH_COLLECT)
		return hot_rehash_st.n_saved;
	return dc->stat.hot_valid_entries;
}

	uint64_t hot_cmt_fill_permille(const demand_cache *dc)
{
	uint64_t max_hot = dc->env.max_cached_hot_entries;
	uint64_t valid = hot_cmt_report_valid_entries(dc);

	if (max_hot == 0)
		return 0;
	return valid * 1000 / max_hot;
}

	uint64_t hot_cmt_warm_fill_permille(const demand_cache *dc)
{
	uint64_t max_hot = dc->env.max_cached_hot_entries;
	uint64_t warm;
	int i;

	if (dc == NULL || max_hot == 0)
		return 0;
	warm = 0;
	for (i = DEFAULT_STATE; i < STATE_NUM; i++)  /* state>=2 */
		warm += dc->stat.hot_pte_state_live[i];
	return warm * 1000 / max_hot;
}

static void hot_rehash_advance_probe(demand_cache *self, int *d_idx, int *p_idx, int probe)
{
	int p = *p_idx + probe * probe;
	int max_tpages = (int)self->env.max_cached_hot_tpages;

	*d_idx = (*d_idx + p / EPP) % max_tpages;
	*p_idx = p % EPP;
}

static int hot_rehash_place_entry_primary(demand_cache *self, const h_pte_t *src)
{
	lpa_t lpa = src->lpa;
	lpa_t new_lpa = lpa % self->env.max_cached_hot_entries;
	int d_idx = (int)(new_lpa / EPP);
	int p_idx = (int)(new_lpa % EPP);
	int probe = 0;
	h_pte_t *slot;

	for (;;)
	{
		slot = &self->member.hot_mem_table[d_idx][p_idx];
		if (slot->lpa == UINT32_MAX || slot->lpa == lpa)
		{
			if (slot->lpa != UINT32_MAX)
				hot_hist_dec(self, slot->state);
			hot_rehash_copy_pte(slot, src);
			hot_hist_inc(self, slot->state);
			return 0;
		}
		probe++;
		if (probe >= HOT_REHASH_MAX_PROBE)
			break;
		hot_rehash_advance_probe(self, &d_idx, &p_idx, probe);
	}
	return -1;
}

static int hot_rehash_place_entry_fallback(demand_cache *self, const h_pte_t *src)
{
	lpa_t lpa = src->lpa;
	h_pte_t *slot;

	for (uint64_t pi = 0; pi < self->env.max_cached_hot_tpages; pi++)
	{
		for (int gi = 0; gi < EPP; gi++)
		{
			slot = &self->member.hot_mem_table[pi][gi];
			if (slot->lpa == UINT32_MAX || slot->lpa == lpa)
			{
				if (slot->lpa != UINT32_MAX)
					hot_hist_dec(self, slot->state);
				hot_rehash_copy_pte(slot, src);
				hot_hist_inc(self, slot->state);
				return 0;
			}
		}
	}
	return -1;
}

static int hot_rehash_place_entry(demand_cache *self, const h_pte_t *src)
{
	if (hot_rehash_place_entry_primary(self, src) == 0)
		return 0;
	return hot_rehash_place_entry_fallback(self, src);
}

static void hot_rehash_finish_locked(demand_cache *self)
{
	if (hot_rehash_st.collect_dropped_cold > 0)
		ftl_log("hot rehash: evicted %" PRIu64 " cold grains (warm-only collect)\n",
			hot_rehash_st.collect_dropped_cold);
	if (hot_rehash_st.insert_dropped > 0)
		ftl_err("hot rehash: insert dropped %" PRIu64 " (saved=%" PRIu64 " placed=%" PRIu64 ")\n",
			hot_rehash_st.insert_dropped, hot_rehash_st.n_saved, hot_rehash_st.placed);

	free(hot_rehash_st.saved);
	hot_rehash_st.saved = NULL;
	hot_rehash_st.saved_cap = 0;
	hot_rehash_st.n_saved = 0;
	hot_rehash_st.insert_idx = 0;
	hot_rehash_st.scan_page = 0;
	hot_rehash_st.scan_grain = 0;
	hot_rehash_st.insert_dropped = 0;
	hot_rehash_st.collect_dropped_cold = 0;
	hot_rehash_st.phase = HOT_REHASH_IDLE;
	self->stat.hot_valid_entries = hot_rehash_st.placed;
	self->member.nr_ghost_key = hot_rehash_st.ghosts;
}

static bool hot_rehash_saved_push(const h_pte_t *pte)
{
	if (hot_rehash_st.n_saved >= hot_rehash_st.saved_cap)
	{
		uint64_t cap = hot_rehash_st.saved_cap ? hot_rehash_st.saved_cap * 2 : 4096;
		hot_rehash_entry_t *next = (hot_rehash_entry_t *)g_realloc(hot_rehash_st.saved,
									   cap * sizeof(*next));

		if (!next)
			return false;
		hot_rehash_st.saved = next;
		hot_rehash_st.saved_cap = cap;
	}
	hot_rehash_st.saved[hot_rehash_st.n_saved++].pte = *pte;
	return true;
}

static void hot_rehash_begin_locked(demand_cache *self)
{
	free(hot_rehash_st.saved);
	memset(&hot_rehash_st, 0, sizeof(hot_rehash_st));
	hot_rehash_st.n_pages = self->env.max_cached_hot_tpages;
	hot_rehash_st.placed = 0;
	hot_rehash_st.ghosts = 0;
	self->stat.hot_valid_entries = 0;
	self->member.nr_ghost_key = 0;
	memset(self->stat.hot_pte_state_live, 0, sizeof(self->stat.hot_pte_state_live));
	hot_rehash_st.phase = HOT_REHASH_COLLECT;
}

void hot_rehash_begin(demand_cache *self)
{
	pthread_spin_lock(&hot_cmt_lock);
	hot_rehash_begin_locked(self);
	pthread_spin_unlock(&hot_cmt_lock);
}

static void hot_rehash_step_locked(demand_cache *self, uint64_t budget)
{
	h_pte_t *slot;
	uint64_t work = 0;

	if (hot_rehash_st.phase == HOT_REHASH_IDLE)
		return;

	if (hot_rehash_st.phase == HOT_REHASH_COLLECT)
	{
		while (work < budget && hot_rehash_st.scan_page < hot_rehash_st.n_pages)
		{
			slot = &self->member.hot_mem_table[hot_rehash_st.scan_page]
							 [hot_rehash_st.scan_grain];
			if (slot->lpa != UINT32_MAX)
			{
				/* Expand/Shrink rehash: warm grains only; drop cold to speed eviction */
				if (hot_pte_is_warm(slot))
				{
					if (!hot_rehash_saved_push(slot))
						ftl_err("hot rehash: saved alloc failed\n");
				}
				else
				{
					hot_rehash_yield_key_budget_on_drop(self, slot);
					hot_rehash_st.collect_dropped_cold++;
				}
				hot_rehash_clear_slot(self, slot);
			}
			work++;
			hot_rehash_st.scan_grain++;
			if (hot_rehash_st.scan_grain >= EPP)
			{
				hot_rehash_st.scan_grain = 0;
				hot_rehash_st.scan_page++;
			}
		}
		if (hot_rehash_st.scan_page >= hot_rehash_st.n_pages)
		{
			hot_rehash_st.insert_idx = 0;
			hot_rehash_st.phase = HOT_REHASH_INSERT;
		}
		return;
	}

	if (hot_rehash_st.phase == HOT_REHASH_INSERT)
	{
		while (work < budget && hot_rehash_st.insert_idx < hot_rehash_st.n_saved)
		{
			const h_pte_t *src = &hot_rehash_st.saved[hot_rehash_st.insert_idx].pte;

			if (hot_rehash_place_entry(self, src) != 0)
			{
				/* Table full (load>=1): count once; do not advance idx */
				hot_rehash_yield_key_budget_on_drop(self, src);
				hot_rehash_st.insert_dropped++;
				hot_rehash_st.insert_idx++;
			}
			else
			{
				hot_rehash_st.placed++;
				if (src->key_ghost_mark)
					hot_rehash_st.ghosts++;
				hot_rehash_st.insert_idx++;
			}
			work++;
		}
		if (hot_rehash_st.insert_idx >= hot_rehash_st.n_saved)
			hot_rehash_finish_locked(self);
	}
}

void hot_rehash_tick(demand_cache *self)
{
	uint64_t budget;

	if (hot_rehash_st.phase == HOT_REHASH_INSERT)
		budget = HOT_REHASH_INSERT_BUDGET;
	else
		budget = HOT_REHASH_SCAN_BUDGET;

	pthread_spin_lock(&hot_cmt_lock);
	hot_rehash_step_locked(self, budget);
	pthread_spin_unlock(&hot_cmt_lock);
}

void hot_rehash_drain(demand_cache *self, uint32_t rounds)
{
	while (rounds-- > 0 && !hot_cmt_accessible())
		hot_rehash_tick(self);
}

void hot_rehash_drain_all(demand_cache *self)
{
	uint64_t steps = 0;

	while (!hot_cmt_accessible())
	{
		hot_rehash_tick(self);
		if (++steps > 200000000ULL)
		{
			ftl_err("hot rehash drain_all timeout phase=%s saved=%" PRIu64
				" idx=%" PRIu64 " placed=%" PRIu64 "\n",
				hot_rehash_phase_name(hot_rehash_st.phase),
				hot_rehash_st.n_saved, hot_rehash_st.insert_idx,
				hot_rehash_st.placed);
			break;
		}
	}
}
#endif /* ADAPTIVE_MEM */

int dftl_cache_hot_reset(demand_cache *d_cache)
{
	for (uint64_t i = 0; i < d_cache->env.max_cached_hot_tpages; i++)
	{
		for (int j = 0; j < EPP; j++)
		{
#ifdef ADAPTIVE_MEM
			d_cache->member.hot_mem_table[i][j].key_ghost_mark = false;
#endif
			d_cache->member.hot_mem_table[i][j].lpa = UINT32_MAX;
			d_cache->member.hot_mem_table[i][j].ppa = UINT32_MAX;
			d_cache->member.hot_mem_table[i][j].state = MIN_STATE;
#ifdef STORE_KEY_FP
			d_cache->member.hot_mem_table[i][j].key_fp = 0;
#endif
#ifdef VERIFY_CACHE
			d_cache->member.hot_mem_table[i][j].real_key.len = 0;
			d_cache->member.hot_mem_table[i][j].real_key.key[0] = 0;
#endif
		}
	}
	memset(d_cache->stat.hot_pte_state_live, 0,
	       sizeof(d_cache->stat.hot_pte_state_live));
	memset(d_cache->stat.hot_pte_state_on_hit, 0,
	       sizeof(d_cache->stat.hot_pte_state_on_hit));
	d_cache->stat.hot_valid_entries = 0;
#ifdef VERIFY_CACHE
	d_cache->env.max_cached_full_key = d_cache->env.max_cached_full_key_budget;
#endif
#ifdef ADAPTIVE_MEM
	d_cache->member.nr_ghost_key = 0;
#endif
	return 0;
}

/* Cold 页 grain 被访问：累加热度并记录统计，供后续晋升使用（不执行晋升） */
void dftl_cache_cold_grain_access(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	uint8_t cnt;

#ifdef ADAPTIVE_MEM
	if (!cmt_is_resident(cmt))
		return;
#else
	if (cmt->pt == NULL)
		return;
#endif
	cnt = cmt_grain_heat_raise(cmt, P_IDX);
	if (!cold_cnt_promotable(cnt) || cmt->pt[P_IDX].ppa == UINT32_MAX)
		return;

	self->stat.up_grain_cnt++;
	self->stat.up_hit_cnt += cnt;
	if (cnt < STATE_NUM)
		self->stat.cold_promote_grain_period[cnt]++;
	self->stat.grain_heat_distribute[cnt]++;
}

int dftl_cache_promote_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim)
{
#ifdef ADAPTIVE_MEM
	if (!hot_cmt_accessible())
	{
		if (victim != NULL)
			cmt_page_heat_clear(victim);
		hot_rehash_drain(self, 8);
		return 0;
	}
	pthread_spin_lock(&hot_cmt_lock);
#endif
#ifdef ADAPTIVE_MEM
	if (victim == NULL || cmt_is_ghost(victim) || victim->pt == NULL || victim->cnt_map == NULL)
#else
	if (victim == NULL || victim->cnt_map == NULL)
#endif
	{
#ifdef ADAPTIVE_MEM
		pthread_spin_unlock(&hot_cmt_lock);
#endif
		return -1;
	}
	{
		uint32_t total_valid_entry = 0;
		int i;

		for (i = 0; i < EPP; i++)
			if (victim->pt[i].ppa != UINT32_MAX)
				total_valid_entry++;
		if (total_valid_entry == 0)
		{
			cmt_page_heat_clear(victim);
#ifdef ADAPTIVE_MEM
			pthread_spin_unlock(&hot_cmt_lock);
			hot_rehash_drain(self, 8);
#endif
			return 0;
		}
	}
	{
		uint32_t page_heat = victim->page_heat_sum;
		int promoted_grains = 0;
		int promotable_grains = 0;

		for (int i = 0; i < EPP; i++)
		{
			uint8_t cnt;

			if (victim->pt[i].ppa == UINT32_MAX)
				continue;
			cnt = victim->cnt_map[i];
			if (cnt < STATE_NUM)
				self->stat.cold_promote_page_grain_period[cnt]++;
			if (!cold_cnt_promotable(cnt))
				continue;
			/*direct mapping may cause rewrite and collision*/
			{
				lpa_t grain_lpa = IDX_TO_LPA(victim->idx, i);

				promotable_grains++;
				self->stat.up_grain_cnt++;
				self->stat.up_hit_cnt += cnt;
				if (cnt < STATE_NUM)
					self->stat.cold_promote_grain_period[cnt]++;
				self->stat.grain_heat_distribute[cnt]++;
				if (hot_promote_place_grain(self, grain_lpa, victim->pt[i].ppa,
							    victim->pt[i].key_fp, cnt))
					promoted_grains++;
			}
		}
		if (promotable_grains > 0)
		{
			self->stat.cold_promote_pages_period++;
			self->stat.cold_promote_page_heat_sum_period += page_heat;
		}
		(void)promoted_grains;
	}
	cmt_page_heat_clear(victim);  /* 清零 page_heat_sum 和 cnt_map */
	cmt_update_hot_entry_flag(victim); /* 晋升后重扫 cnt_map，更新 has_hot_entry */

	/* 新方案：只减去晋升 grain 的热度，保留其他 grain 的 cnt_map */
	/* 晋升后不改变 LRU 位置，依赖 page_heat_sum 减少避免重复晋升 */

	self->stat.up_page_cnt++;
#ifdef ADAPTIVE_MEM
	pthread_spin_unlock(&hot_cmt_lock);
#endif
	return 1;
}

bool dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, h_pte_t **hot_pte)
{
	bool hit;

#ifdef ADAPTIVE_MEM
	if (!hot_cmt_accessible())
	{
		hot_rehash_drain(self, 4);
		return false;
	}
	pthread_spin_lock(&hot_cmt_lock);
#endif
	lpa_t new_lpa = lpa % (self->env.max_cached_hot_entries);
	int max_tpages = (int)self->env.max_cached_hot_tpages;
	int d_idx = (int)(new_lpa / EPP);
	int p_idx = (int)(new_lpa % EPP);
	int probe = 0;

	while (probe < MAX_PROBE)
	{
		if (self->member.hot_mem_table[d_idx][p_idx].lpa == lpa)
		{
			*hot_pte = &self->member.hot_mem_table[d_idx][p_idx];
			hot_state_raise(self, *hot_pte);
			hit = true;
			goto hot_hit_out;
		}
		probe++;
		if (probe >= MAX_PROBE)
			break;
		hot_probe_advance(&d_idx, &p_idx, probe, max_tpages);
	}
	hit = false;
hot_hit_out:
#ifdef ADAPTIVE_MEM
	pthread_spin_unlock(&hot_cmt_lock);
	if (!hit)
		hot_rehash_drain(self, 4);
#endif
	return hit;
}
#endif

int dftl_cache_load(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	struct inflight_params *i_params;

	if (IS_INITIAL_PPA(cmt->t_ppa))
	{
		return 0;
	}

	i_params = get_iparams(req, wb_entry);
	i_params->jump = GOTO_LIST;
	self->stat.cache_load++;
	uint64_t lat = __demand.li->read(cmt->t_ppa, PAGESIZE, 0);
	if (req)
	{
		req->etime = clock_get_ns() + lat;
	}
	else if (wb_entry)
	{
		wb_entry->etime = clock_get_ns() + lat;
	}
	else
	{
		abort();
	}
	cmt->is_flying = true;
	return 1;
}

int dftl_cache_list_up(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	int rc = 0;
	struct cmt_struct *cmt = self->member.cmt[D_IDX];

	algorithm *palgo = self->env.palgo;
	w_buffer_t *pw_buffer = D_ENV(palgo)->pw_buffer;
	struct cmt_struct *victim = NULL;
	struct inflight_params *i_params;
	static int pop_count = 0;
	if (self->is_full(self))
	{
		pop_count++;
		{
			int pop_guard = 0;

			do
			{
				if (++pop_guard > (int)self->env.max_cached_tpages + 64)
				{
					ftl_err("list_up: no evictable victim (ghost-only LRU?)\n");
					abort();
				}
#ifdef HOT_CMT
				{
				bool if_promote = (COLD_LRU_PROMOTE_INTERVAL != 0 &&
						   pop_count % COLD_LRU_PROMOTE_INTERVAL == 0);
				if (if_promote)
				{
					lru_promote_result_t res;
					struct cmt_struct *promote_cmt;

					lru_pick_promote_hot_pages(self->member.lru, &res,
							       COLD_LRU_PROMOTE_TOP_K);
					for (int pi = 0; pi < res.count; pi++)
					{
						promote_cmt = (struct cmt_struct *)res.candidates[pi];
						if (promote_cmt != NULL && promote_cmt->pt != NULL &&
						    promote_cmt->cnt_map != NULL
#ifdef ADAPTIVE_MEM
						    && !cmt_is_ghost(promote_cmt)
#endif
						)
							self->promote_hot(self, UINT32_MAX, NULL, NULL,
									  promote_cmt);
					}
				}
				}
#endif /* HOT_CMT */
				victim = (struct cmt_struct *)lru_pop(self->member.lru);
				if (!victim)
					abort();
				victim->lru_ptr = NULL;
#ifdef ADAPTIVE_MEM
				if (cmt_is_ghost(victim) || victim->pt == NULL)
					continue;
#else
				if (victim->pt == NULL || victim->cnt_map == NULL)
					continue;
#endif
				break;
			} while (1);
		}
		cmt_cold_tail_evict_teardown(victim);
		self->member.nr_cached_tpages--;
		victim->pt = NULL;
		if (victim->state == DIRTY)
		{
			self->stat.dirty_evict++;

			i_params = get_iparams(req, wb_entry);
			i_params->jump = GOTO_COMPLETE;
			// i_params->pte = cmbr->mem_table[D_IDX][P_IDX];

			victim->t_ppa = tp_alloc(pbm);
			pbm->validate_page(pbm, victim->t_ppa);
			victim->state = CLEAN;

			// struct pt_struct pte = cmbr->mem_table[D_IDX][P_IDX];

			uint64_t lat = __demand.li->write(victim->t_ppa, PAGESIZE, 0);
			if (req)
			{
				req->etime = clock_get_ns() + lat;
			}
			else if (wb_entry)
			{
				wb_entry->etime = clock_get_ns() + lat;
			}
			else
			{
				abort();
			}

			bm_oob_t new_oob = {
				.is_tpage = true,
				.lpa = victim->idx,
				.length = PAGESIZE};
			pbm->set_oob(pbm, victim->t_ppa * GRAIN_PER_PAGE, &new_oob);

			rc = 1;
		}
		else
		{
			self->stat.clean_evict++;
		}
#ifdef ADAPTIVE_MEM
		{
			static uint32_t rebalance_evict_cnt;

			adaptive_ghost_cold_mark(self, victim);
			if (!hot_cmt_accessible())
				hot_rehash_drain(self, 16);
			else if ((++rebalance_evict_cnt & 63) == 0)
				adaptive_try_rebalance(self);
		}
#endif
	}
#ifdef ADAPTIVE_MEM
	adaptive_ghost_cold_clear(self, cmt);
#endif
	cmt->pt = self->member.mem_table[D_IDX];
	if (!cmt->cnt_map)
		cmt->cnt_map = (uint8_t *)g_malloc0(sizeof(uint8_t) * EPP);
	else
		memset(cmt->cnt_map, 0, sizeof(uint8_t) * EPP);
	cmt_cold_resident_meta_init(cmt);
	if (cmt->lru_ptr)
		lru_update(self->member.lru, cmt->lru_ptr);
	else
		cmt->lru_ptr = lru_push(self->member.lru, (void *)cmt);
	self->member.nr_cached_tpages++;

	if (cmt->is_flying)
	{
		ensure_cmt_queues(cmt);
		cmt->is_flying = false;
		if (req)
		{
			request *retry_req;
			while (ring_dequeue(cmt->retry_q, (void *)&retry_req, 1))
			{
				struct inflight_params *i_params = get_iparams(retry_req, NULL);
				i_params->jump = GOTO_COMPLETE;
				algo_q_insert_sorted(palgo->retry_q, retry_req, NULL);
			}
		}
		else if (wb_entry)
		{
			snode *retry_wbe;
			while (ring_dequeue(cmt->retry_q, (void *)&retry_wbe, 1))
			{
				// lpa_t retry_lpa = get_lpa(retry_wbe->key, retry_wbe->hash_params);

				struct inflight_params *i_params = get_iparams(NULL, retry_wbe);
				i_params->jump = GOTO_COMPLETE;
				// i_params->pte = cmt->pt[OFFSET(retry_lpa)];
				algo_q_insert_sorted(pw_buffer->env->wb_retry_q, NULL, retry_wbe);
			}
		}
	}

	return rc;
}

int dftl_cache_wait_if_flying(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	if (cmt->is_flying)
	{
		ensure_cmt_queues(cmt);
		self->stat.blocked_miss++;

		if (req)
			while (!ring_enqueue(cmt->retry_q, (void *)&req, 1))
				;
		else if (wb_entry)
			while (!ring_enqueue(cmt->retry_q, (void *)&wb_entry, 1))
				;
		else
			abort();

		return 1;
	}
	return 0;
}

int dftl_cache_touch(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
#ifdef ADAPTIVE_MEM
	if (!cmt_is_resident(cmt) || cmt->lru_ptr == NULL)
		return 0;
#else
	if (cmt->lru_ptr == NULL)
		return 0;
#endif
	lru_update(self->member.lru, cmt->lru_ptr);
	return 0;
}

int dftl_cache_update(demand_cache *self, lpa_t lpa, struct pt_struct pte)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];

	if (cmt->pt)
	{
		cmt->pt[P_IDX] = pte;

		if (!IS_INITIAL_PPA(cmt->t_ppa) && cmt->state == CLEAN)
		{
			pbm->invalidate_page(pbm, cmt->t_ppa);
			cmt->t_ppa = UINT32_MAX;
		}
		cmt->state = DIRTY;
		lru_update(self->member.lru, cmt->lru_ptr);
	}
	else
	{
		/* FIXME: to handle later update after evict */
		self->member.mem_table[D_IDX][P_IDX] = pte;
	}
	return 0;
}

struct pt_struct dftl_cache_get_pte(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	if (cmt->pt)
	{
		return cmt->pt[P_IDX];
	}
	else
	{
		/* FIXME: to handle later update after evict */
		return self->member.mem_table[D_IDX][P_IDX];
	}
}

struct cmt_struct *dftl_cache_get_cmt(demand_cache *self, lpa_t lpa)
{
	return self->member.cmt[D_IDX];
}

bool dftl_cache_is_hit(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
#ifdef ADAPTIVE_MEM
	if (cmt_is_resident(cmt))
#else
	if (cmt->pt != NULL)
#endif
	{
#ifdef HOT_CMT
		dftl_cache_cold_grain_access(self, lpa);
#endif
		return 1;
	}
	return 0;
}

bool dftl_cache_is_full(demand_cache *self)
{
	return (self->member.nr_cached_tpages >= self->env.max_cached_tpages);
}