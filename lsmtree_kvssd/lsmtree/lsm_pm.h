#ifndef __LSM_PM_H__
#define __LSM_PM_H__
#include "lsm_bm.h"

typedef struct page_t
{
    bm_superblock_t *data_active, *data_reserve;
    bm_superblock_t *map_active, *map_reserve;
} page_t;

int lsm_page_init(block_mgr_t *bm);
ppa_t lsm_get_page(block_mgr_t *bm, int type);
bool validate_PPA(uint8_t type, uint32_t ppa);
bool invalidate_PPA(uint8_t type, uint32_t ppa);

// for GC
int gc_header(block_mgr_t *bm);
int gc_data(block_mgr_t *bm);
#endif //__LSM_PM_H__