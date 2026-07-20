#ifndef __BM_H__
#define __BM_H__

#include <stdbool.h>
#include "lsm_settings.h"

/* Defines */
#define SBLK_IDX(x) ((x) / _PPS)
#define INSBLK_OFFSET(x) ((x) % _PPS)
#define SBLK_END (_PPS)
#define SBLK_OFFT2PPA(sblk, offt) (sblk->index * _PPS + offt)
#define PPA2SBLK(ppa) (ppa / PIECE_PER_PAGE / _PPS)

enum
{
    MAP = 0,
    DATA = 1,
};

/* Structures */
typedef struct bm_oob_t
{
    uint8_t length;
    // 0 free,1 valid,-1invalid
    bool isvalid;
} bm_oob_t;

typedef struct bm_pg_oob_t
{
    bm_oob_t *piece_oob;
} bm_pg_oob_t;

typedef struct bm_superblock_t
{
    const uint32_t index;
    uint32_t invalid_cnt; // in pieces
    uint32_t wp_offt;     // write point offset
} bm_superblock_t;

typedef struct bm_sblk_master_t
{
    uint32_t min_sblk_idx;
    uint32_t max_sblk_idx;
    uint32_t next_free_sblk;
    uint32_t free_sblk_cnt;
    uint32_t active_sblk;
    uint32_t reserve_sblk;
} bm_sblk_master_t;

typedef struct bm_env_t
{
    uint16_t map_sblk_num;
    uint16_t data_sblk_num;
    uint64_t piece_cnt;
    // bool *valid_bitmap;
    // for superblock
    bm_sblk_master_t *sblk_master;
    bm_superblock_t *sblk;
    bm_oob_t *piece_oob;
} bm_env_t;

typedef struct block_mgr_t
{
    void (*create)(struct block_mgr_t *bm);
    void (*destroy)(struct block_mgr_t *bm);

    bool (*isvalid_piece)(struct block_mgr_t *bm, ppa_t piece);
    void (*validate_piece)(struct block_mgr_t *bm, ppa_t piece);
    void (*invalidate_piece)(struct block_mgr_t *bm, ppa_t piece);
    bool (*isvalid_page)(struct block_mgr_t *bm, ppa_t ppa);    // only used for tpages
    void (*validate_page)(struct block_mgr_t *bm, ppa_t ppa);   // only used for tpages
    void (*invalidate_page)(struct block_mgr_t *bm, ppa_t ppa); // only used for tpages
    bool (*check_full)(struct block_mgr_t *bm, bm_superblock_t *sblk);
    bool (*isgc_needed)(struct block_mgr_t *bm, int type); // parted

    // void (*trim_segment)(struct block_mgr_t *bm, bm_superblock_t *sblk, lower_info *li);

    bm_superblock_t *(*change_reserve)(struct block_mgr_t *bm, int type, bm_superblock_t *reserve);
    bm_superblock_t *(*get_gc_target)(struct block_mgr_t *, int type);
    bm_superblock_t *(*get_active_superblock)(struct block_mgr_t *bm, int type, bool isreserve);
    ppa_t (*get_page_num)(struct block_mgr_t *bm, bm_superblock_t *sblk);

    void (*set_oob)(struct block_mgr_t *bm, ppa_t piece, bm_oob_t *oob);
    bm_oob_t *(*get_oob)(struct block_mgr_t *bm, ppa_t piece);
    bm_env_t *env;
} block_mgr_t;
#endif // __BM_H__