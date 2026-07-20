#ifndef __DFTL_LRU_LIST__
#define __DFTL_LRU_LIST__

#include <stdbool.h>
#include <stdint.h>

#define LRU_PROMOTE_MAX_K 100

typedef struct __node {
  void *DATA;
  struct __node *next;
  struct __node *prev;
} NODE;

typedef struct __lru {
  int size;
  NODE *head;
  NODE *tail;
} LRU;

typedef struct {
  void *candidates[LRU_PROMOTE_MAX_K];
  int count;
} lru_promote_result_t;

// lru
void lru_init(LRU **);
void lru_free(LRU *);
NODE *lru_push(LRU *, void *);
void *lru_pop(LRU *);
void lru_update(LRU *, NODE *);
void lru_delete(LRU *, NODE *);
NODE *lru_get_target_node(LRU *lru, NODE *now);
void *lru_pop_hot(LRU *lru);
/* LRU 头部 top_pct% 中选取 cnt_map 页总热度 Top-K（不移除节点） */
void lru_pick_promote_top_k(LRU *lru, lru_promote_result_t *out, int k);
void *lru_pick_promote_top_heat(LRU *lru);
/* 扫描 LRU 前 10%，选取 has_hot_entry=true 的页面，够 target_k 页即停止 */
void lru_pick_promote_hot_pages(LRU *lru, lru_promote_result_t *out, int target_k);

#endif
