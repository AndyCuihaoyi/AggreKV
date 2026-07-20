#ifndef __BM_H__
#define __BM_H__

#include "dftl_types.h"
#include "dftl_utils.h"

#define SBLK_IDX(x) ((x) / _PPS)
#define INSBLK_OFFSET(x) ((x) % _PPS)
#define SBLK_END (_PPS)
#define SBLK_OFFT2PPA(sblk, offt) (sblk->index * _PPS + offt)
#ifdef DATA_SEGREGATION
/* MAX_GC_STREAM 定义在 dftl_types.h 中 */
#define PPA2SBLK_IDX(ppa) ((ppa) / _PPS)
#endif
enum {
  MAP_S = 0,
  DATA_S = 1,
};

typedef enum {
  DATA,
  MAP,
} page_t;

/* Structures */
typedef struct bm_oob_t {
  bool is_tpage;
  lpa_t lpa;
  uint32_t length;
} bm_oob_t;

typedef struct bm_superblock_t { // length = segment = _PPS
  const uint32_t index;
  uint32_t valid_cnt; // in grains
  uint32_t wp_offt;
  /* 累计 GC 次数：本 sblk 作为 victim 被选中做 GC 的次数（含 trim） */
  uint64_t gc_cnt;
#ifdef DATA_SEGREGATION
  /* is_owned: 本 sblk 被某流独占（wb path 只能写到 active 流拥有的 sblk）
   * is_rsv:   本 sblk 处于 rsv 池（trim 后水位维护到 ≥ MAX_GC_STREAM，
   *           切流时优先从此池拿）。二者互斥。 */
  bool is_owned;
  uint32_t stream_idx;
  bool is_rsv;
#endif
} bm_superblock_t;

typedef struct bm_part_ext_t {
  uint32_t s_sblk;   // start superblock
  uint32_t e_sblk;   // end superblock
  uint32_t sblk_rsv; // reserved superblock (MAP_S 模式保留)
  uint32_t free_sblk_cnt; // 通用空闲 sblk 数（DATA 模式下 = !is_rsv && !is_owned）
  uint32_t active_sblk;
#ifdef DATA_SEGREGATION
  uint32_t rsv_sblk_cnt; // rsv 池水位（trim 后流入，恒 ≥ MAX_GC_STREAM）
#endif
} bm_part_ext_t;

#ifdef DATA_SEGREGATION
/* DS 模式下流专属的状态机。与单流 dftl_get_active_sblk+get_page_num 协作：
 *   - 切流时：active_sblk 换到新 sblk，active_sblk_offset 重置为 0
 *   - wb/GC dp_alloc 推 wp_offt 时，active_sblk_offset 同步++（保持 = wp_offt）
 *   - page_remain：当前 batch 还剩多少 page 才满
 *   - grain_remain：当前 page 还剩多少 grain
 *   - flush_ppa / flush_page：旧 batch 的 [起点, 长度)。flush 时把当前 batch
 *     内已写入的 [flush_ppa, flush_ppa + flush_page) 一次性提交。flush 完成后
 *     flush_ppa = 当前 active_ppa（next-to-write），flush_page = 0。 */
typedef struct bm_stream_manager_t {
  uint32_t idx;
  uint32_t active_sblk_offset; /* 同步于 active_sblk->wp_offt */
  ppa_t active_ppa;            /* = SBLK_OFFT2PPA(active_sblk, active_sblk_offset) */
  ppa_t flush_ppa;             /* 旧 batch 起点 */
  uint32_t flush_page;         /* 旧 batch 已写 page 数 */
  uint32_t page_remain;        /* 旧 batch 还剩多少 page */
  uint32_t grain_remain;       /* 当前 page 还剩多少 grain */
  bm_superblock_t *active_sblk;
} bm_stream_manager_t;

#endif

typedef struct bm_env_t {
  uint64_t grain_cnt;
  bool *valid_bitmap;
  bm_oob_t *oob;

  // for superblock
  int part_num;
  bm_part_ext_t *part;
  bm_superblock_t *sblk;
#ifdef DATA_SEGREGATION
  bm_stream_manager_t *stream;
#endif
} bm_env_t;

typedef struct block_mgr_t {
  bm_env_t *env;

  void (*create)(struct block_mgr_t *bm);
  void (*destroy)(struct block_mgr_t *bm);

  bool (*isvalid_grain)(struct block_mgr_t *bm, ppa_t grain);
  void (*validate_grain)(struct block_mgr_t *bm, ppa_t grain);
  void (*invalidate_grain)(struct block_mgr_t *bm, ppa_t grain);
  bool (*isvalid_page)(struct block_mgr_t *bm,
                       ppa_t ppa); // only used for tpages
  void (*validate_page)(struct block_mgr_t *bm,
                        ppa_t ppa); // only used for tpages
  void (*invalidate_page)(struct block_mgr_t *bm,
                          ppa_t ppa); // only used for tpages
  bool (*check_full)(struct block_mgr_t *bm, bm_superblock_t *sblk);
  bool (*isgc_needed)(struct block_mgr_t *bm, int pt_num); // parted

  void (*trim_segment)(struct block_mgr_t *bm, bm_superblock_t *sblk,
                       lower_info *li);

  bm_superblock_t *(*change_reserve)(struct block_mgr_t *bm, int pt_num,
                                     bm_superblock_t *reserve);
  bm_superblock_t *(*get_gc_target)(struct block_mgr_t *, int pt_num,
                                    int stream_idx);
  bm_superblock_t *(*get_active_superblock)(struct block_mgr_t *bm, int pt_num,
                                            bool isreserve);
  ppa_t (*get_page_num)(struct block_mgr_t *bm, bm_superblock_t *sblk);

  void (*set_oob)(struct block_mgr_t *bm, ppa_t grain, bm_oob_t *oob);
  bm_oob_t *(*get_oob)(struct block_mgr_t *bm, ppa_t grain);
  void (*show_sblk_state)(struct block_mgr_t *self, int pt_num);
#ifdef DATA_SEGREGATION
  bm_superblock_t *(*get_stream_superblock)(struct block_mgr_t *bm, lpa_t lpa,
                                            bool is_reserve);

#endif
} block_mgr_t;

/* 全局 block manager 实例（定义在 dftl_bm.c） */
extern block_mgr_t *pbm;

/* 打印所有 sblk 的 GC 擦除次数（磨损均衡统计） */
void bm_show_gc_cnt_stats(block_mgr_t *bm);

#endif // __BM_H__