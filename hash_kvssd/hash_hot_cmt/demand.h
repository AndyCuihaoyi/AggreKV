#ifndef __DFTL_H__
#define __DFTL_H__

#include "../tools/lru_list.h"
#include "../tools/rte_ring/rte_ring.h"
#include "bm.h"
#include "cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "write_buffer.h"
#include <stdint.h>
#include <string.h>

extern uint64_t extra_mem_lat;

#define ENTRY_SIZE 8

#define EPP (PAGESIZE / ENTRY_SIZE) // Number of table entries per page
#define D_IDX (lpa / EPP)           // Idx of directory table
#define P_IDX (lpa % EPP)           // Idx of page table
#ifdef HOT_CMT
#define D_IDX_HOT (new_lpa / EPP)
#define P_IDX_HOT (new_lpa % EPP)
#define IDX_TO_LPA(d_idx, p_idx) ((d_idx)*EPP + (p_idx))
#endif

#define CLEAN 0
#define DIRTY 1

#define QUADRATIC_PROBING(h, c) ((h) + (c) + (c) * (c))
#define LINEAR_PROBING(h, c) (h + c)

#define PROBING_FUNC(h, c) QUADRATIC_PROBING(h, c)

#define D_ENV(p_algo) ((demand_env *)(p_algo->env))

// Page table entry
typedef struct __attribute__((packed)) pt_struct {
  ppa_t ppa; // Index = lpa
#ifdef STORE_KEY_FP
  fp_t key_fp;
#endif
} pte_t;

// Hot page table entry：固定 3bit / 6 档（0-5）
#define STATE_BITS 3
#define STATE_NUM 6
#define MIN_STATE 0
#define HOT_STATE_MAX (STATE_NUM - 1)
#define DEFAULT_STATE 2 /* 温：新晋升默认 */
#define HOT_STATE_MIN 3 /* 热：3-5 */
/* adaptive rebalance：Hot 表被挤占 victim 的 state
 * 3-5→Hot_Evict，0-2→Cold_Evict */
#define ADAPTIVE_HOT_EVICT_STATE_THRESH DEFAULT_STATE
#define HOT_EVICT_STATE_THRESH ADAPTIVE_HOT_EVICT_STATE_THRESH

/* Hot PTE state 与 Cold CMT cnt_map[grain] 共用上述 3bit 六档（0-5） */
static inline void grain_heat_raise(uint8_t *heat) {
  if (*heat < HOT_STATE_MAX)
    (*heat)++;
}

static inline void grain_heat_decay(uint8_t *heat) {
  if (*heat > MIN_STATE)
    (*heat)--;
}

#define COLD_CNT_PROMOTE_THRESH 1 /* cnt>=1（访问 1 次）即尝试 promote */
#define COLD_LRU_PROMOTE_TOP_PCT 50 /* LRU 头部 5% 窗口内选页 */
#define COLD_LRU_PROMOTE_INTERVAL                                              \
  1000 /* 每 2000 次 Cold LRU 满时：晋升 + 尾部淘汰 */
#define COLD_LRU_PROMOTE_TOP_K 50 /* 每次晋升从 Top-K 候选中选 */

static inline uint8_t cmt_counter_raise(uint8_t *counter) {
  grain_heat_raise(counter);
  return *counter;
}

static inline bool cold_cnt_promotable(uint8_t cnt) {
  return cnt >= COLD_CNT_PROMOTE_THRESH;
}

typedef struct __attribute__((packed)) hot_pt_struct {
  lpa_t lpa;
  ppa_t ppa;                  // Index = lpa
  uint8_t state : STATE_BITS; /* 3bit 六档，与 Cold cnt_map 同刻度 */
#ifdef VERIFY_CACHE
  KEYT real_key;
#endif
#ifdef STORE_KEY_FP
  fp_t key_fp;
#endif
#ifdef ADAPTIVE_MEM
  bool
      key_ghost_mark; /* Selective Key evicted; key_fp retains ghost identity */
  uint8_t key_ghost_state
      : STATE_BITS; /* Ghost Key 命中频次档位（0=cold-5=hot） */
#endif
} h_pte_t;

/* rehash COLLECT 保留 state>=2 */
static inline bool hot_pte_is_warm(const h_pte_t *slot) {
  return slot != NULL && slot->lpa != UINT32_MAX &&
         slot->state >= DEFAULT_STATE;
}

static inline bool hot_pte_is_hot(const h_pte_t *slot) {
  return slot != NULL && slot->lpa != UINT32_MAX &&
         slot->state >= HOT_STATE_MIN;
}

/* promote 挤占统计：仅看被换出 Hot 槽的 state（3-5 热驱逐 / 0-2 冷驱逐） */
static inline bool hot_pte_counts_hot_evict(const h_pte_t *slot) {
  return slot != NULL && slot->lpa != UINT32_MAX &&
         slot->state >= HOT_EVICT_STATE_THRESH;
}

static inline bool promote_displace_counts_hot_evict(const h_pte_t *victim,
                                                     uint8_t incoming_cnt) {
  (void)incoming_cnt;
  return hot_pte_counts_hot_evict(victim);
}

extern KEYT *real_keys;

// Cache mapping table data strcuture
typedef struct cmt_struct {
  int32_t idx;
  pte_t *pt;
  ppa_t t_ppa;

  bool state; // CLEAN / DIRTY
  bool is_flying;

  struct rte_ring *retry_q;
  struct rte_ring *wait_q;
  NODE *lru_ptr;
  bool *is_cached;
  uint32_t cached_cnt;
  uint32_t dirty_cnt;

  uint8_t *cnt_map;
  /* Σ cnt_map[i]，命中 raise 时增量更新，供 LRU 晋升选页 */
  uint32_t page_heat_sum;
  bool has_hot_entry; /* 页面内有 cnt_map[i] >= DEFAULT_STATE 的 entry */
#ifdef ADAPTIVE_MEM
  bool ghost_mark; /* Cold mapping page evicted; heavy meta (pt/cnt_map)
                      released */
#endif
} cmt_t;

#ifdef ADAPTIVE_MEM
static inline bool cmt_cnt_map_active(const cmt_t *cmt) {
  return cmt != NULL && cmt->pt != NULL && cmt->cnt_map != NULL;
}
#else
static inline bool cmt_cnt_map_active(const cmt_t *cmt) {
  return cmt != NULL && cmt->cnt_map != NULL;
}
#endif

/* 单 grain 热度 +1，同步维护 page_heat_sum */
static inline uint8_t cmt_grain_heat_raise(cmt_t *cmt, int grain_idx) {
  uint8_t prev;
  uint8_t next;

  if (!cmt_cnt_map_active(cmt) || grain_idx < 0 || grain_idx >= EPP)
    return 0;
  prev = cmt->cnt_map[grain_idx];
  next = cmt_counter_raise(&cmt->cnt_map[grain_idx]);
  if (next > prev)
    cmt->page_heat_sum += (uint32_t)(next - prev);
  //  if (next >= DEFAULT_STATE)
  cmt->has_hot_entry = true;
  return next;
}

/* 单 grain 热度清零（晋升成功后扣减 page_heat_sum） */
static inline void cmt_grain_heat_clear(cmt_t *cmt, int grain_idx) {
  uint8_t prev;

  if (!cmt_cnt_map_active(cmt) || grain_idx < 0 || grain_idx >= EPP)
    return;
  prev = cmt->cnt_map[grain_idx];
  if (prev == 0)
    return;
  cmt->cnt_map[grain_idx] = 0;
  if (cmt->page_heat_sum >= (uint32_t)prev)
    cmt->page_heat_sum -= (uint32_t)prev;
  else
    cmt->page_heat_sum = 0;
}

/* 晋升后重新扫描 cnt_map，更新 has_hot_entry 标志 */
static inline void cmt_update_hot_entry_flag(cmt_t *cmt) {
  if (!cmt_cnt_map_active(cmt))
    return;
  for (int i = 0; i < EPP; i++) {
    if (cmt->pt[i].ppa != UINT32_MAX && cmt->cnt_map[i] >= DEFAULT_STATE) {
      cmt->has_hot_entry = true;
      return;
    }
  }
  cmt->has_hot_entry = false;
}

static inline void cmt_page_heat_clear(cmt_t *cmt) {
  if (cmt == NULL)
    return;
  cmt->page_heat_sum = 0;
  if (cmt->cnt_map != NULL)
    memset(cmt->cnt_map, 0, sizeof(uint8_t) * EPP);
}

/* LRU 尾部淘汰：清零页级热度（cnt_map 由 ghost/换出路径释放） */
static inline void cmt_cold_tail_evict_teardown(cmt_t *cmt) {
  if (cmt == NULL)
    return;
  cmt->page_heat_sum = 0;
}

static inline void cmt_cold_resident_meta_init(cmt_t *cmt) {
  if (cmt == NULL)
    return;
  cmt->page_heat_sum = 0;
  cmt->has_hot_entry = false;
}

/* Hot 命中且 Cold 页仍驻留：累加 cnt_map + page_heat_sum */
static inline void cold_cnt_record_grain(cmt_t *cmt, int grain_idx) {
  cmt_grain_heat_raise(cmt, grain_idx);
}

#ifdef ADAPTIVE_MEM
static inline bool cmt_is_ghost(const cmt_t *cmt) {
  return cmt != NULL && cmt->ghost_mark;
}

static inline bool cmt_is_resident(const cmt_t *cmt) {
  return cmt != NULL && cmt->pt != NULL && !cmt->ghost_mark;
}

static inline bool hot_pte_key_is_ghost(const h_pte_t *slot) {
  return slot != NULL && slot->key_ghost_mark;
}
#endif

typedef struct demand_env {
  uint32_t num_page;
  uint32_t num_grain;
  uint32_t max_cache_entry;
  uint32_t num_block;
  uint32_t p_p_b;
  uint32_t num_tblock;
  uint32_t num_tpage;
  uint32_t num_dblock;
  uint32_t num_dpage;
  uint32_t num_dgrain;
  uint32_t nr_pages_optimal_caching;
  uint32_t num_max_cache;
  uint32_t real_max_cache;
  uint32_t max_write_buf;
  uint32_t max_try;

  /* for statistics */
  uint64_t num_rd_wb_hit;
  uint64_t num_rd_data_rd;
  uint64_t num_rd_data_miss_rd;
  uint64_t num_data_gc;
  uint64_t num_gc_flash_read;
  uint64_t gc_cmt_total;
  uint64_t num_gc_mapping_hit;
  uint64_t num_gc_flash_write;
  uint64_t r_hash_collision_cnt[MAX_HASH_COLLISION + 1];
  uint64_t w_hash_collision_cnt[MAX_HASH_COLLISION + 1];

  /* components */
  demand_cache *pd_cache;
  w_buffer_t *pw_buffer;
  block_mgr_t *pb_mgr;
} demand_env;

/* extern variables */
extern algorithm __demand;
extern demand_env d_env;

// dftl.c
uint32_t demand_create(algorithm *, lower_info *);
void demand_destroy(algorithm *, lower_info *);
lpa_t get_lpa(demand_cache *pd_cache, KEYT key, void *_h_params);
uint32_t demand_set(algorithm *, request *const);
uint32_t demand_get(algorithm *, request *const);
uint32_t demand_remove(algorithm *, request *const); // not implemented

// dftl_range.c
uint32_t demand_range_query(algorithm *, request *const);
bool range_end_req(algorithm *, request *);

// dftl_utils.c
void cache_show(char *dest);

#endif // __DFTL_H__
