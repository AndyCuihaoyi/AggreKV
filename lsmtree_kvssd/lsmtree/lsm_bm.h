#ifndef __LSM_BM_H__
#define __LSM_BM_H__

#include "bm.h"

void lsm_bm_init(block_mgr_t *self);
void lsm_sblk_print(block_mgr_t *self);
void lsm_bm_free(block_mgr_t *self);
bm_oob_t *lsm_bm_get_oob(block_mgr_t *self, ppa_t piece);
void lsm_bm_set_oob(block_mgr_t *self, ppa_t piece, bm_oob_t *oob);
bool lsm_check_full(block_mgr_t *self, bm_superblock_t *sblk);
ppa_t lsm_get_page_num(block_mgr_t *self, bm_superblock_t *sblk);
bm_superblock_t *lsm_get_active_sblk(block_mgr_t *self, int pt_num,
                                     bool isreserve);
bool lsm_isgc_needed(block_mgr_t *self, int pt_num);
bool lsm_isvalid_piece(block_mgr_t *self, ppa_t piece);
void lsm_validate_piece(block_mgr_t *self, ppa_t piece);
void lsm_invalidate_piece(block_mgr_t *self, ppa_t piece);
bool lsm_isvalid_page(block_mgr_t *self, ppa_t ppa);
void lsm_validate_page(block_mgr_t *self, ppa_t ppa);
void lsm_invalidate_page(block_mgr_t *self, ppa_t ppa);
bm_superblock_t *lsm_get_gc_target(block_mgr_t *self, int pt_num);
// void lsm_trim_segment(block_mgr_t *self, bm_superblock_t *sblk, lower_info *li);
bm_superblock_t *lsm_change_reserve(block_mgr_t *self, int pt_num, bm_superblock_t *reserve);

#endif // __LSM_BM_H__