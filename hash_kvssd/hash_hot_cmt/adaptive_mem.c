/*
 * Adaptive memory management: in-place Ghost marks (Phase 1) + chunk rebalance
 * skeleton.
 *
 * Ghost Cold: cmt_t.ghost_mark on evicted slots; pt/cnt_map released (no
 * metadata). Ghost Key:  h_pte_t.key_ghost_mark; real_key released, key_fp
 * retains identity. FIFO rings track eviction order only when ghost count
 * reaches cap.
 */

#include "cache.h"
#include "demand.h"
#include "dftl_settings.h"
#include <glib.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ADAPTIVE_MEM

static pthread_spinlock_t adaptive_mem_lock;

typedef struct {
  uint32_t *ring;
  uint64_t cap;
  uint64_t head;
  uint64_t count;
} ghost_fifo_t;

static ghost_fifo_t cold_fifo;
static ghost_fifo_t key_fifo;

#define GHOST_FIFO_MAX 65536
/* FIFO 失效时 round-robin 扫描步数（禁止 O(nr_valid_tpages) 全表扫描） */
#define GHOST_FALLBACK_SCAN_STEPS 512

static uint64_t ghost_cold_scan_rr;
static uint64_t ghost_key_scan_rr;

static uint64_t ghost_fifo_cap(uint64_t ghost_cap) {
  if (ghost_cap == 0)
    return 0;
  return ghost_cap < GHOST_FIFO_MAX ? ghost_cap : GHOST_FIFO_MAX;
}

typedef struct {
  uint64_t ghost_hit_cold;
  uint64_t ghost_hit_key;
  uint64_t hot_evict_count;
  uint64_t cold_evict_count;
  uint64_t expand_count;
  uint64_t shrink_count;
} adaptive_stat_snap_t;

static void adaptive_ghost_compute_caps(demand_cache *dc) {
  struct cache_env *env = &dc->env;
  uint64_t hot_bytes;
  uint64_t key_bytes;
  uint64_t stolen_bytes;

  hot_bytes = env->max_cached_hot_tpages * (uint64_t)PAGESIZE;
  key_bytes = env->max_cached_full_key_budget;
  stolen_bytes = hot_bytes + key_bytes;
  env->ghost_cold_cap = stolen_bytes / sizeof(uint32_t);
  if (env->ghost_cold_cap == 0)
    env->ghost_cold_cap = 1;
  env->ghost_cold_frac_pct = (uint64_t)(env->hot_cmt_frac * 100.0 + 0.5);
#ifdef VERIFY_CACHE
  env->ghost_cold_frac_pct += (uint64_t)(env->full_key_frac * 100.0 + 0.5);
#endif

#ifdef VERIFY_CACHE
  {
    uint64_t key_budget_entries = env->max_cached_full_key_budget / MAXKEYSIZE;
    if (env->max_cached_hot_entries > key_budget_entries)
      env->ghost_key_cap =
          env->max_cached_hot_entries - key_budget_entries;
    else
      env->ghost_key_cap = 0;
  }
#else
  env->ghost_key_cap = 0;
#endif
}

static int adaptive_ghost_fifo_alloc(ghost_fifo_t *gf, uint64_t cap) {
  if (cap == 0) {
    free(gf->ring);
    gf->ring = NULL;
    gf->cap = 0;
    gf->head = 0;
    gf->count = 0;
    return 0;
  }

  uint32_t *next = (uint32_t *)calloc(cap, sizeof(uint32_t));
  if (!next) {
    ftl_err("ghost fifo alloc failed, cap=%" PRIu64 "\n", cap);
    abort();
  }
  free(gf->ring);
  gf->ring = next;
  gf->cap = cap;
  gf->head = 0;
  gf->count = 0;
  return 0;
}

static void adaptive_ghost_fifo_free(ghost_fifo_t *gf) {
  free(gf->ring);
  gf->ring = NULL;
  gf->cap = 0;
  gf->head = 0;
  gf->count = 0;
}

static void adaptive_ghost_fifo_push(ghost_fifo_t *gf, uint32_t val) {
  if (gf->cap == 0 || !gf->ring)
    return;
  gf->ring[gf->head] = val;
  gf->head = (gf->head + 1) % gf->cap;
  if (gf->count < gf->cap)
    gf->count++;
}

static uint32_t adaptive_ghost_fifo_pop_oldest(ghost_fifo_t *gf) {
  uint64_t pos;

  if (gf->count == 0)
    return UINT32_MAX;
  pos = (gf->head + gf->cap - gf->count) % gf->cap;
  gf->count--;
  return gf->ring[pos];
}

static void ghost_cold_unmark(cmt_t *cmt, demand_cache *dc) {
  cmt->ghost_mark = false;
  cmt->page_heat_sum = 0;
  if (cmt->cnt_map) {
    g_free(cmt->cnt_map);
    cmt->cnt_map = NULL;
  }
  if (dc->member.nr_ghost_cold > 0)
    dc->member.nr_ghost_cold--;
}

static bool ghost_cold_evict_rr(demand_cache *dc) {
  uint64_t n = dc->env.nr_valid_tpages;
  uint64_t steps = GHOST_FALLBACK_SCAN_STEPS;

  if (n == 0 || dc->member.nr_ghost_cold == 0)
    return false;

  while (steps-- > 0) {
    cmt_t *cmt = dc->member.cmt[ghost_cold_scan_rr++ % n];

    if (cmt && cmt->ghost_mark) {
      ghost_cold_unmark(cmt, dc);
      return true;
    }
  }
  return false;
}

static bool ghost_cold_fifo_evict_oldest(demand_cache *dc) {
  uint32_t tpn;
  cmt_t *cmt;
  uint64_t tries = cold_fifo.count;

  while (tries-- > 0 && cold_fifo.count > 0) {
    tpn = adaptive_ghost_fifo_pop_oldest(&cold_fifo);
    if (tpn >= dc->env.nr_valid_tpages)
      continue;
    cmt = dc->member.cmt[tpn];
    if (cmt && cmt->ghost_mark) {
      ghost_cold_unmark(cmt, dc);
      return true;
    }
  }
  return ghost_cold_evict_rr(dc);
}

static uint32_t hot_slot_id(uint64_t page, uint64_t grain) {
  return (uint32_t)(page * EPP + grain);
}

static void hot_slot_decode(uint32_t id, uint64_t *page, uint64_t *grain) {
  *page = id / EPP;
  *grain = id % EPP;
}

static bool ghost_key_evict_rr(demand_cache *dc) {
  uint64_t hot_slots = dc->env.max_cached_hot_tpages * EPP;
  uint64_t steps = GHOST_FALLBACK_SCAN_STEPS;

  if (hot_slots == 0 || dc->member.nr_ghost_key == 0)
    return false;

  while (steps-- > 0) {
    uint64_t pos = ghost_key_scan_rr++ % hot_slots;
    uint64_t page = pos / EPP;
    uint64_t grain = pos % EPP;
    h_pte_t *slot = &dc->member.hot_mem_table[page][grain];

    if (slot->key_ghost_mark) {
      uint8_t state =
          slot->key_ghost_state < STATE_NUM ? slot->key_ghost_state : 0;
      dc->stat.ghost_key_state_live[state]--;
      slot->key_ghost_mark = false;
      slot->key_ghost_state = 0;
      dc->member.nr_ghost_key--;
      return true;
    }
  }
  return false;
}

static bool ghost_key_fifo_evict_oldest(demand_cache *dc) {
  uint32_t slot_id;
  uint64_t page;
  uint64_t grain;
  h_pte_t *slot;
  uint64_t tries = key_fifo.count;

  while (tries-- > 0 && key_fifo.count > 0) {
    slot_id = adaptive_ghost_fifo_pop_oldest(&key_fifo);
    hot_slot_decode(slot_id, &page, &grain);
    if (page >= dc->env.max_cached_hot_tpages)
      continue;
    slot = &dc->member.hot_mem_table[page][grain];
    if (slot->key_ghost_mark) {
      uint8_t state =
          slot->key_ghost_state < STATE_NUM ? slot->key_ghost_state : 0;
      dc->stat.ghost_key_state_live[state]--;
      slot->key_ghost_mark = false;
      slot->key_ghost_state = 0;
      dc->member.nr_ghost_key--;
      return true;
    }
  }
  return ghost_key_evict_rr(dc);
}

static void adaptive_ghost_trim_locked(demand_cache *dc) {
  while (dc->member.nr_ghost_cold > dc->env.ghost_cold_cap) {
    if (!ghost_cold_fifo_evict_oldest(dc))
      break;
  }
  while (dc->member.nr_ghost_key > dc->env.ghost_key_cap) {
    if (!ghost_key_fifo_evict_oldest(dc))
      break;
  }
}

static void adaptive_ghost_update_caps_locked(demand_cache *dc) {
  adaptive_ghost_compute_caps(dc);
  adaptive_ghost_fifo_alloc(&cold_fifo, ghost_fifo_cap(dc->env.ghost_cold_cap));
  adaptive_ghost_fifo_alloc(&key_fifo, ghost_fifo_cap(dc->env.ghost_key_cap));
  adaptive_ghost_trim_locked(dc);
}

static const char *adaptive_event_name(adaptive_event_t ev) {
  switch (ev) {
  case ADAPTIVE_EV_INIT:
    return "INIT";
  case ADAPTIVE_EV_EXPAND:
    return "EXPAND";
  case ADAPTIVE_EV_SHRINK:
    return "SHRINK";
  default:
    return "SUMMARY";
  }
}

void adaptive_mem_init(void) {
  pthread_spin_init(&adaptive_mem_lock, PTHREAD_PROCESS_PRIVATE);
}

void adaptive_ghost_destroy(void) {
  adaptive_ghost_fifo_free(&cold_fifo);
  adaptive_ghost_fifo_free(&key_fifo);
}

void adaptive_stats_bootstrap(demand_cache *dc) {
  adaptive_mem_init();
  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.ghost_hit_cold = 0;
  dc->stat.ghost_hit_key = 0;
  dc->stat.hot_evict_count = 0;
  dc->stat.cold_evict_count = 0;
  dc->stat.expand_count = 0;
  dc->stat.shrink_count = 0;
  dc->member.nr_ghost_cold = 0;
  dc->member.nr_ghost_key = 0;
  adaptive_ghost_update_caps_locked(dc);
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_ghost_cold_mark(demand_cache *dc, cmt_t *cmt) {
  if (!cmt || dc->env.ghost_cold_cap == 0)
    return;

  pthread_spin_lock(&adaptive_mem_lock);
  if (cmt->ghost_mark) {
    pthread_spin_unlock(&adaptive_mem_lock);
    return;
  }
  while (dc->member.nr_ghost_cold >= dc->env.ghost_cold_cap) {
    if (!ghost_cold_fifo_evict_oldest(dc))
      break;
  }
  cmt->ghost_mark = true;
  cmt->page_heat_sum = 0;
  if (cmt->cnt_map) {
    g_free(cmt->cnt_map);
    cmt->cnt_map = NULL;
  }
  dc->member.nr_ghost_cold++;
  adaptive_ghost_fifo_push(&cold_fifo, (uint32_t)cmt->idx);
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_ghost_cold_clear(demand_cache *dc, cmt_t *cmt) {
  if (!cmt || !cmt->ghost_mark)
    return;

  pthread_spin_lock(&adaptive_mem_lock);
  if (cmt->ghost_mark) {
    cmt->ghost_mark = false;
    dc->member.nr_ghost_cold--;
  }
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_ghost_cold_on_miss(demand_cache *dc, cmt_t *cmt, bool is_update) {
  if (!cmt || !cmt->ghost_mark || !is_update)
    return;

  __atomic_add_fetch(&dc->stat.ghost_hit_cold, 1, __ATOMIC_RELAXED);
}

void adaptive_ghost_key_mark(demand_cache *dc, h_pte_t *slot, uint64_t hot_page,
                             uint64_t hot_grain) {
  if (!slot || dc->env.ghost_key_cap == 0)
    return;
#ifdef VERIFY_CACHE
  if (slot->real_key.len == 0)
    return;
#endif

  pthread_spin_lock(&adaptive_mem_lock);
  if (slot->key_ghost_mark) {
    pthread_spin_unlock(&adaptive_mem_lock);
    return;
  }
  while (dc->member.nr_ghost_key >= dc->env.ghost_key_cap) {
    if (!ghost_key_fifo_evict_oldest(dc))
      break;
  }
  slot->key_ghost_mark = true;
  slot->key_ghost_state = 0;
  dc->stat.ghost_key_state_live[0]++;
#ifdef VERIFY_CACHE
  if (slot->real_key.len != 0 &&
      dc->env.max_cached_full_key < dc->env.max_cached_full_key_budget)
    dc->env.max_cached_full_key += slot->real_key.len;
  slot->real_key.len = 0;
  slot->real_key.key[0] = 0;
#endif
  dc->member.nr_ghost_key++;
  adaptive_ghost_fifo_push(&key_fifo, hot_slot_id(hot_page, hot_grain));
  pthread_spin_unlock(&adaptive_mem_lock);
}

bool adaptive_ghost_key_on_flash_verify(demand_cache *dc, h_pte_t *hot_pte,
                                        fp_t key_fp) {
  if (!hot_pte || key_fp == 0 || dc->env.ghost_key_cap == 0)
    return false;
  if (!hot_pte->key_ghost_mark || hot_pte->key_fp != key_fp)
    return false;

  pthread_spin_lock(&adaptive_mem_lock);
  if (hot_pte->key_ghost_mark && hot_pte->key_fp == key_fp) {
    dc->stat.ghost_hit_key++;
    uint8_t old_state = hot_pte->key_ghost_state;
    if (hot_pte->key_ghost_state < STATE_NUM - 1)
      hot_pte->key_ghost_state++;
    uint8_t new_state = hot_pte->key_ghost_state;
    if (old_state < STATE_NUM)
      dc->stat.ghost_key_state_live[old_state]--;
    dc->stat.ghost_key_state_live[new_state]++;
  }
  pthread_spin_unlock(&adaptive_mem_lock);
  return true;
}

static void adaptive_stat_snapshot(const demand_cache *dc,
                                   adaptive_stat_snap_t *snap) {
  pthread_spin_lock(&adaptive_mem_lock);
  snap->ghost_hit_cold = dc->stat.ghost_hit_cold;
  snap->ghost_hit_key = dc->stat.ghost_hit_key;
  snap->hot_evict_count = dc->stat.hot_evict_count;
  snap->cold_evict_count = dc->stat.cold_evict_count;
  snap->expand_count = dc->stat.expand_count;
  snap->shrink_count = dc->stat.shrink_count;
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_reset_stats(demand_cache *dc) {
  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.ghost_hit_cold = 0;
  dc->stat.ghost_hit_key = 0;
  dc->stat.hot_evict_count = 0;
  dc->stat.cold_evict_count = 0;
  pthread_spin_unlock(&adaptive_mem_lock);
  dc->stat.hot_promote_blocked_cnt = 0;
}

void adaptive_prepare_for_benchmark(demand_cache *dc) {
  uint64_t i;

  pthread_spin_lock(&adaptive_mem_lock);
  for (i = 0; i < dc->env.nr_valid_tpages; i++) {
    cmt_t *cmt = dc->member.cmt[i];

    if (cmt && cmt->ghost_mark) {
      cmt->ghost_mark = false;
      cmt->page_heat_sum = 0;
    }
  }
  dc->member.nr_ghost_cold = 0;
  dc->member.nr_ghost_key = 0;
  if (cold_fifo.ring) {
    cold_fifo.head = 0;
    cold_fifo.count = 0;
  }
  if (key_fifo.ring) {
    key_fifo.head = 0;
    key_fifo.count = 0;
  }
  dc->stat.ghost_hit_cold = 0;
  dc->stat.ghost_hit_key = 0;
  dc->stat.hot_evict_count = 0;
  dc->stat.cold_evict_count = 0;
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_stat_inc_hot_evict(demand_cache *dc) {
  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.hot_evict_count++;
  pthread_spin_unlock(&adaptive_mem_lock);
}

void adaptive_stat_inc_cold_evict(demand_cache *dc) {
  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.cold_evict_count++;
  pthread_spin_unlock(&adaptive_mem_lock);
}

static uint64_t adaptive_hot_empty_grains(const demand_cache *dc) {
  uint64_t valid = hot_cmt_report_valid_entries(dc);
  uint64_t max_hot = dc->env.max_cached_hot_entries;

  if (max_hot <= valid)
    return 0;
  return max_hot - valid;
}

static uint64_t cache_partition_bytes_cold(const demand_cache *dc) {
  return dc->env.max_cached_tpages * (uint64_t)PAGESIZE;
}

static uint64_t cache_partition_bytes_hot(const demand_cache *dc) {
  return dc->env.max_cached_hot_tpages * (uint64_t)PAGESIZE;
}

static uint64_t cache_partition_bytes_key(const demand_cache *dc) {
  return dc->env.max_cached_full_key_budget;
}

static void adaptive_clear_hot_page(demand_cache *dc, uint64_t page_idx) {
  for (int j = 0; j < EPP; j++) {
    h_pte_t *slot = &dc->member.hot_mem_table[page_idx][j];

    hot_cmt_hist_on_clear_slot(dc, slot);
    if (slot->lpa != UINT32_MAX && dc->stat.hot_valid_entries > 0)
      dc->stat.hot_valid_entries--;
    if (slot->key_ghost_mark) {
      uint8_t state =
          slot->key_ghost_state < STATE_NUM ? slot->key_ghost_state : 0;
      dc->stat.ghost_key_state_live[state]--;
      slot->key_ghost_mark = false;
      slot->key_ghost_state = 0;
      if (dc->member.nr_ghost_key > 0)
        dc->member.nr_ghost_key--;
    }
    slot->lpa = UINT32_MAX;
    slot->ppa = UINT32_MAX;
    slot->state = MIN_STATE;
#ifdef STORE_KEY_FP
    slot->key_fp = 0;
#endif
#ifdef VERIFY_CACHE
    if (slot->real_key.len != 0 &&
        dc->env.max_cached_full_key < dc->env.max_cached_full_key_budget)
      dc->env.max_cached_full_key += slot->real_key.len;
    slot->real_key.len = 0;
    slot->real_key.key[0] = 0;
#endif
  }
}

static int adaptive_expand_key_cache(demand_cache *dc, uint64_t key_free) {
  struct cache_env *env = &dc->env;
  if (key_free != 0)
    return 0;
  if (env->max_cached_full_key_budget >= env->key_entries_cap)
    return 0;

  uint64_t old_budget = env->max_cached_full_key_budget;
  uint64_t new_budget = old_budget + env->key_chunk_entries;
  if (new_budget > env->key_entries_cap)
    new_budget = env->key_entries_cap;

  /* 从 Cold CMT 扣除 Key Cache 扩容所需的页数 */
  uint64_t key_chunk_bytes = env->key_chunk_entries;
  uint64_t key_chunk_pages = (key_chunk_bytes + PAGESIZE - 1) / PAGESIZE;
  if (key_chunk_pages == 0)
    key_chunk_pages = 1;
  if (env->max_cached_tpages <= env->min_cached_tpages + key_chunk_pages - 1)
    return 0;

  env->max_cached_tpages -= key_chunk_pages;

  env->max_cached_full_key_budget = new_budget;
  /* free slots = new budget - used; 扩容后 used 不变，所以 free 增加 */
  env->max_cached_full_key = new_budget - (old_budget - key_free);

  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.expand_count++;
  adaptive_ghost_update_caps_locked(dc);
  pthread_spin_unlock(&adaptive_mem_lock);

  char detail[256];
  snprintf(detail, sizeof(detail),
           "expand Key Cache: free=%" PRIu64 "->%" PRIu64 " bytes, budget %" PRIu64
           "->%" PRIu64 " bytes, cold_tpages-=%" PRIu64,
           key_free, env->max_cached_full_key, old_budget, new_budget,
           key_chunk_pages);
  adaptive_show_stats_event(dc, ADAPTIVE_EV_EXPAND, detail);
  adaptive_reset_stats(dc);
  return 1;
}

static int adaptive_expand_hot_from_cold(demand_cache *dc, const char *detail) {
  struct cache_env *env = &dc->env;
  uint64_t chunk = env->chunk_tpages;
  uint64_t old_hot = env->max_cached_hot_tpages;

  if (env->max_cached_tpages <= env->min_cached_tpages + chunk - 1)
    return 0;
  if (old_hot + chunk > env->hot_tpages_cap)
    return 0;

  env->max_cached_tpages -= chunk;
  env->max_cached_hot_tpages += chunk;
  env->max_cached_hot_entries = env->max_cached_hot_tpages * EPP;

  hot_cmt_table_lock();
  for (uint64_t i = old_hot; i < env->max_cached_hot_tpages; i++)
    adaptive_clear_hot_page(dc, i);
  hot_cmt_table_unlock();
  hot_rehash_begin(dc);
  hot_rehash_drain_all(dc);

  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.expand_count++;
  adaptive_ghost_update_caps_locked(dc);
  pthread_spin_unlock(&adaptive_mem_lock);
  adaptive_show_stats_event(dc, ADAPTIVE_EV_EXPAND, detail);
  adaptive_reset_stats(dc);
  return 1;
}

static int adaptive_shrink_hot_to_cold(demand_cache *dc, const char *detail) {
  struct cache_env *env = &dc->env;
  uint64_t chunk = env->chunk_tpages;
  uint64_t old_hot = env->max_cached_hot_tpages;

  if (old_hot <= env->min_cached_hot_tpages + chunk - 1)
    return 0;
  if (env->max_cached_tpages + chunk > env->total_cache_tpages)
    return 0;

  hot_cmt_table_lock();
  for (uint64_t i = old_hot - chunk; i < old_hot; i++)
    adaptive_clear_hot_page(dc, i);
  env->max_cached_hot_tpages -= chunk;
  env->max_cached_hot_entries = env->max_cached_hot_tpages * EPP;
  env->max_cached_tpages += chunk;
  hot_cmt_table_unlock();
  hot_rehash_begin(dc);
  hot_rehash_drain_all(dc);

  pthread_spin_lock(&adaptive_mem_lock);
  dc->stat.shrink_count++;
  adaptive_ghost_update_caps_locked(dc);
  pthread_spin_unlock(&adaptive_mem_lock);
  adaptive_show_stats_event(dc, ADAPTIVE_EV_SHRINK, detail);
  adaptive_reset_stats(dc);
  return 1;
}

static int adaptive_hot_expand_blocked(const struct cache_env *env) {
  return env->max_cached_hot_tpages + env->chunk_tpages > env->hot_tpages_cap;
}

static void adaptive_log_expand_cap_once(const struct cache_env *env) {
  static uint64_t last_hot_tpages;

  if (last_hot_tpages == env->max_cached_hot_tpages)
    return;
  last_hot_tpages = env->max_cached_hot_tpages;
  ftl_log("adaptive: Hot at expand cap (%" PRIu64 "/%" PRIu64
          " tpages, %d%% of pool "
          "%" PRIu64
          "); expand blocked until Hot_Evict drops (shrink via evict stats)\n",
          env->max_cached_hot_tpages, env->hot_tpages_cap,
          ADAPTIVE_HOT_MAX_POOL_PCT, env->total_cache_tpages);
}

/*
 * Expand — hot_fill ≥ 500‰（state≥2 条目 > 50%）
 * Shrink — hot_fill < 250‰ 且 (Cold_Evict 占优 或 低利用)
 */
void adaptive_try_rebalance(demand_cache *dc) {
  struct cache_env *env = &dc->env;
  adaptive_stat_snap_t snap;
  uint64_t hot_cmt_evict;
  uint64_t hot_fill;
  uint64_t total_fill_permille;
  uint64_t blocked_permille;
  uint64_t total_pressure;
  char detail[384];
  int shrink_cold_pressure;
  int shrink_low_util;
  int shrink_wanted;
  int at_cap;
  uint64_t key_free;

  if (!hot_cmt_accessible()) {
    hot_rehash_drain(dc, HOT_REHASH_DRAIN_ROUNDS);
    return;
  }

  adaptive_stat_snapshot(dc, &snap);
  hot_cmt_evict = snap.hot_evict_count + snap.cold_evict_count;
  hot_fill = hot_cmt_warm_fill_permille(dc);
  total_fill_permille = hot_cmt_fill_permille(dc);
  blocked_permille = env->max_cached_hot_entries
                         ? dc->stat.hot_promote_blocked_cnt * 1000 /
                               env->max_cached_hot_entries
                         : 0;
  total_pressure = hot_fill + blocked_permille;
  at_cap = adaptive_hot_expand_blocked(env);

  shrink_cold_pressure = snap.hot_evict_count < env->n_hot &&
                         snap.cold_evict_count > snap.hot_evict_count &&
                         hot_cmt_evict > env->n_hot;
  shrink_low_util = snap.hot_evict_count < env->n_hot && hot_fill <= 250;
  shrink_wanted = hot_fill < 250 && (shrink_cold_pressure || shrink_low_util);

  if (total_pressure > 500 && !at_cap) {
    snprintf(detail, sizeof(detail),
             "expand Hot<-Cold: hot_fill+blocked=%" PRIu64 "‰>500"
             " (hot_fill=%" PRIu64 "‰ blocked=%" PRIu64 "‰"
             " Hot_Evict=%" PRIu64 " Cold_Evict=%" PRIu64 ")",
             total_pressure, hot_fill, blocked_permille, snap.hot_evict_count,
             snap.cold_evict_count);
    if (adaptive_expand_hot_from_cold(dc, detail))
      return;
  } else if (total_pressure > 500 && at_cap)
    adaptive_log_expand_cap_once(env);

  key_free = env->max_cached_full_key;
  if (key_free == 0 && adaptive_expand_key_cache(dc, key_free))
    return;

  if (shrink_wanted) {
    if (shrink_cold_pressure)
      snprintf(detail, sizeof(detail),
               "shrink Hot->Cold [cold-pressure]: Hot_Evict=%" PRIu64
               " < N_hot=%" PRIu64 " AND Cold_Evict=%" PRIu64
               " > Hot_Evict AND hot_fill=%" PRIu64 "‰ < 250",
               snap.hot_evict_count, env->n_hot, snap.cold_evict_count,
               hot_fill);
    else
      snprintf(detail, sizeof(detail),
               "shrink Hot->Cold [low-util]: hot_fill=%" PRIu64
               "‰ < 250 (random/low-heat; no Cold>Hot required)",
               hot_fill);
    adaptive_shrink_hot_to_cold(dc, detail);
  }
}

void adaptive_show_stats_event(const demand_cache *dc, adaptive_event_t ev,
                               const char *detail) {
  const struct cache_env *env = &dc->env;
  adaptive_stat_snap_t snap;
  uint64_t cold_b = cache_partition_bytes_cold(dc);
  uint64_t hot_b = cache_partition_bytes_hot(dc);
  uint64_t key_b = cache_partition_bytes_key(dc);
  uint64_t total_b = cold_b + hot_b + key_b;
  uint64_t key_used;
  uint64_t empty_grains;
  double cold_pct = total_b ? (100.0 * (double)cold_b / (double)total_b) : 0.0;
  double hot_pct = total_b ? (100.0 * (double)hot_b / (double)total_b) : 0.0;
  double key_pct = total_b ? (100.0 * (double)key_b / (double)total_b) : 0.0;
  uint64_t cold_fill;
  uint64_t key_fill;

  adaptive_stat_snapshot(dc, &snap);
  empty_grains = adaptive_hot_empty_grains(dc);
  key_used = env->max_cached_full_key_budget > env->max_cached_full_key
                 ? env->max_cached_full_key_budget - env->max_cached_full_key
                 : 0;

  pthread_spin_lock(&adaptive_mem_lock);
  cold_fill = dc->member.nr_ghost_cold;
  key_fill = dc->member.nr_ghost_key;
  pthread_spin_unlock(&adaptive_mem_lock);

  ftl_log("========== Adaptive Memory [%s] ==========\n",
          adaptive_event_name(ev));
  if (detail && detail[0])
    ftl_log("  reason: %s\n", detail);
  ftl_log("partition (cold+hot pool %" PRIu64
          " tpages; key budget fixed at init):\n",
          env->total_cache_tpages);
  ftl_log("  Cold CMT:      %6.2f%%  %" PRIu64 " pages, %.2f MiB\n", cold_pct,
          env->max_cached_tpages, (double)cold_b / (1024.0 * 1024.0));
  ftl_log("  Hot CMT:       %6.2f%%  %" PRIu64 " pages, %.2f MiB "
          "(hot_valid=%" PRIu64 " report_valid=%" PRIu64 " fill=%" PRIu64 "‰ "
          "empty_grains~=%" PRIu64 " rehash=%s)\n",
          hot_pct, env->max_cached_hot_tpages,
          (double)hot_b / (1024.0 * 1024.0), dc->stat.hot_valid_entries,
          hot_cmt_report_valid_entries(dc), hot_cmt_fill_permille(dc),
          empty_grains, hot_rehash_phase_name(hot_rehash_phase()));
  ftl_log("  Key Cache:     %6.2f%%  budget %" PRIu64 " used %" PRIu64
          " free %" PRIu64 " bytes (MiB %.2f; %s)\n",
          key_pct, env->max_cached_full_key_budget, key_used,
          env->max_cached_full_key, (double)key_b / (1024.0 * 1024.0),
#ifdef VERIFY_CACHE
          "VERIFY_CACHE on"
#else
          "disabled (budget returned to Cold/Hot pool)"
#endif
  );
  ftl_log("chunk delta=%d%%: N_hot=%" PRIu64 " hot_cap=%" PRIu64 "/%" PRIu64
          " tpages "
          "(%d%% pool)\n",
          ADAPTIVE_CHUNK_FRAC_PCT, env->n_hot, env->max_cached_hot_tpages,
          env->hot_tpages_cap, ADAPTIVE_HOT_MAX_POOL_PCT);
  ftl_log("ghost marks (in-place): cold cap=%" PRIu64 " fill=%" PRIu64
          " (hot+key stolen %.2f MiB, frac=%" PRIu64 "%%) | key cap=%" PRIu64
          " fill=%" PRIu64 "\n",
          env->ghost_cold_cap, cold_fill,
          (double)(env->max_cached_hot_tpages * PAGESIZE +
                   env->max_cached_full_key_budget) /
              (1024.0 * 1024.0),
          env->ghost_cold_frac_pct, env->ghost_key_cap, key_fill);
  ftl_log("rebalance: expand=%" PRIu64 " shrink=%" PRIu64 "\n",
          snap.expand_count, snap.shrink_count);
  ftl_log(
      "period counters (since last rebalance; hot_rewrite_entries is lifetime, "
      "see show_stats):\n");
  ftl_log("  Ghost_Hit_Cold=%" PRIu64 " Ghost_Hit_Key=%" PRIu64 "\n",
          snap.ghost_hit_cold, snap.ghost_hit_key);
  ftl_log("  Hot_Evict=%" PRIu64 " Cold_Evict=%" PRIu64 "\n",
          snap.hot_evict_count, snap.cold_evict_count);
}

void adaptive_show_stats(const demand_cache *dc) {
  adaptive_show_stats_event(dc, ADAPTIVE_EV_SUMMARY, NULL);
}

void adaptive_log_rebalance_period(const demand_cache *dc, const char *tag) {
  const struct cache_env *env = &dc->env;
  adaptive_stat_snap_t snap;
  uint64_t key_used, key_free;

  adaptive_stat_snapshot(dc, &snap);
  key_used = env->max_cached_full_key_budget > env->max_cached_full_key
                 ? env->max_cached_full_key_budget - env->max_cached_full_key
                 : 0;
  key_free = env->max_cached_full_key;

  ftl_log("  [%s adaptive] period total_fill=%" PRIu64 "‰"
          " hot_fill(state≥2)=%" PRIu64 "‰ blocked=%" PRIu64
          " GhostCold=%" PRIu64 " GhostKey=%" PRIu64 "\n",
          tag ? tag : "batch", hot_cmt_fill_permille(dc),
          hot_cmt_warm_fill_permille(dc), dc->stat.hot_promote_blocked_cnt,
          snap.ghost_hit_cold, snap.ghost_hit_key);
  ftl_log("  [%s adaptive] Key Cache: budget %" PRIu64 " used %" PRIu64
          " free %" PRIu64 " bytes\n",
          tag ? tag : "batch", env->max_cached_full_key_budget, key_used,
          key_free);
}

#endif /* ADAPTIVE_MEM */
