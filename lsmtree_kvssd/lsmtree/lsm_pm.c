#include "lsm_pm.h"
#include "../lsm_tools/sem_lock.h"
#include "../lower/ssd.h"
#include "lsmtree.h"
page_t page;
extern bm_env_t bm_env;
extern lsmtree LSM;
fdriver_lock_t gc_wait;
void gc_general_wait_init()
{
    fdriver_lock(&gc_wait);
}

int lsm_page_init(block_mgr_t *bm)
{
    page.data_reserve = bm->get_active_superblock(bm, DATA, true);
    page.data_active = NULL;

    page.map_reserve = bm->get_active_superblock(bm, MAP, true);
    page.map_active = NULL;
    bm_env.piece_oob = (bm_oob_t *)malloc(sizeof(bm_oob_t) * _NOP * PIECE_PER_PAGE);
    for (int i = 0; i < _NOP * PIECE_PER_PAGE; i++)
    {
        bm_env.piece_oob[i].isvalid = 0;
        bm_env.piece_oob[i].length = 0;
    }
    return 1;
}

ppa_t lsm_get_page(block_mgr_t *bm, int type)
{
    ppa_t ppa;
    if (type == MAP)
    {
        if (!page.map_active || bm->check_full(bm, page.map_active))
        {
            // if (bm->isgc_needed(bm, MAP))
            // {
            //     printf("MAP GC\n");
            // }
            //else
            {
                page.map_active = bm->get_active_superblock(bm, MAP, false);
            }
        }
        ppa = bm->get_page_num(bm, page.map_active);
        return ppa;
    }
    else if (type == DATA)
    {
        if (!page.data_active || bm->check_full(bm, page.data_active))
        {
            // if (bm->isgc_needed(bm, DATA))
            // {
            //     printf("DATA GC\n");
            // }
            //else
            {
                page.data_active = bm->get_active_superblock(bm, DATA, false);
            }
        }
        ppa = bm->get_page_num(bm, page.data_active);
        return ppa;
    }
}

bool invalidate_PPA(uint8_t type, uint32_t ppa)
{
    uint32_t t_p = ppa;
    void *t;
    switch (type)
    {
    case DATA:
        bm_env.sblk[PPA2SBLK(ppa)].invalid_cnt += bm_env.piece_oob[ppa].length;
        bm_env.piece_oob[ppa].isvalid = -1;
        break;
    case MAP:
        break;
    default:
        printf("error in validate_ppa\n");
        abort();
    }
    return true;
}

bool validate_PPA(uint8_t type, uint32_t ppa)
{
    uint32_t t_p = ppa;
    switch (type)
    {
    case DATA:
        bm_env.piece_oob[ppa].isvalid = 0;
        break;
    case MAP:
        break;
    default:
        printf("error in validate_ppa\n");
        abort();
    }
    return true;
}

int gc_header(block_mgr_t *bm)
{
    int32_t total_lat = 0;
    bm_superblock_t *gc_target = bm->get_gc_target(bm, MAP);
    ppa_t ppa = SBLK_OFFT2PPA(gc_target, 0);
    ppa_t nppa = lsm_get_page(bm, MAP);
    total_lat += LSM.algo->li->read((ppa), PAGESIZE, 0);
    total_lat += LSM.algo->li->write((nppa), PAGESIZE, 0);
    return total_lat;
}

int gc_data(block_mgr_t *bm)
{
    // 锁定信号量
    int32_t total_lat = 0;
    bm_superblock_t *gc_target = bm->get_gc_target(bm, DATA);
    for (int i = 0; i < _PPS; i++)
    {
        ppa_t ppa = SBLK_OFFT2PPA(gc_target, i);
        ppa_t nppa = lsm_get_page(bm, DATA);
        total_lat += LSM.algo->li->read((ppa), PAGESIZE, 0);
        total_lat += LSM.algo->li->write((nppa), PAGESIZE, 0);
    }
    return total_lat;
}
