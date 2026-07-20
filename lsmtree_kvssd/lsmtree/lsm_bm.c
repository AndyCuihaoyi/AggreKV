#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib-2.0/glib.h>
#include "lsmtree.h"
#include "lsm_bm.h"

extern lsmtree LSM;
extern lower_info ssd_li;
bm_env_t bm_env = {
    .piece_cnt = PIECE_PER_PAGE * _NOP};

block_mgr_t bm = {
    .env = &bm_env,
    .create = lsm_bm_init,
    .destroy = lsm_bm_free,
    .check_full = lsm_check_full,
    .get_page_num = lsm_get_page_num,
    .get_active_superblock = lsm_get_active_sblk,
    .isgc_needed = lsm_isgc_needed,
    .get_gc_target = lsm_get_gc_target};

void lsm_bm_init(block_mgr_t *self)
{
    bm_env_t *env = self->env;
    env->sblk = (bm_superblock_t *)g_malloc0(sizeof(bm_superblock_t) * _NOS);
    for (int i = 1; i <= _NOS; i++)
    {
        *(uint32_t *)(&(env->sblk[i])) = i; // sblk->index = i(const)
        env->sblk[i].invalid_cnt = 0;
    }
    env->sblk_master =
        (bm_sblk_master_t *)g_malloc0(sizeof(bm_sblk_master_t) * PARTNUM);
    // for sblk master
    env->sblk_master[MAP].min_sblk_idx = 1;
    env->sblk_master[MAP].max_sblk_idx = LSM.lsp.HEADERNUM * PAGESIZE / (_PPS * PAGESIZE)*2;
    env->sblk_master[MAP].free_sblk_cnt = env->sblk_master[MAP].max_sblk_idx - env->sblk_master[MAP].min_sblk_idx + 1;
    env->sblk_master[MAP].active_sblk = -1;
    env->sblk_master[MAP].reserve_sblk = env->sblk_master[MAP].min_sblk_idx;
    env->sblk_master[MAP].next_free_sblk = -1;

    env->sblk_master[DATA].min_sblk_idx = env->sblk_master[MAP].max_sblk_idx + 1;
    env->sblk_master[DATA].max_sblk_idx = _NOS;
    env->sblk_master[DATA].free_sblk_cnt = _NOS - env->sblk_master[DATA].min_sblk_idx;
    env->sblk_master[DATA].active_sblk = -1;
    env->sblk_master[DATA].reserve_sblk = env->sblk_master[DATA].min_sblk_idx;
    env->sblk_master[DATA].next_free_sblk = -1;
    //lsm_sblk_print(self);
}

void lsm_sblk_print(block_mgr_t *self)
{
    bm_env_t *env = self->env;
    printf("| ----------------MAP PART\n");
    printf("min sblk index:%d \n", env->sblk_master[MAP].min_sblk_idx);
    printf("max sblk index:%d \n", env->sblk_master[MAP].max_sblk_idx);
    printf("free sblk cnt:%d \n", env->sblk_master[MAP].free_sblk_cnt);
    printf("active sblk index:%d \n", env->sblk_master[MAP].active_sblk);
    printf("reserve sblk index:%d \n", env->sblk_master[MAP].reserve_sblk);
    printf("next sblk index:%d \n", env->sblk_master[MAP].next_free_sblk);
    printf("| ----------------DATA PART\n");
    printf("min sblk index:%d \n", env->sblk_master[DATA].min_sblk_idx);
    printf("max sblk index:%d \n", env->sblk_master[DATA].max_sblk_idx);
    printf("free sblk cnt:%d \n", env->sblk_master[DATA].free_sblk_cnt);
    printf("active sblk index:%d \n", env->sblk_master[DATA].active_sblk);
    printf("reserve sblk index:%d \n", env->sblk_master[DATA].reserve_sblk);
    printf("next sblk index:%d \n", env->sblk_master[DATA].next_free_sblk);
}

void lsm_bm_free(block_mgr_t *self)
{
    bm_env_t *env = self->env;
    for (int i = 0; i < _NOP; i++)
    {
        g_free(env->piece_oob);
    }
    g_free(env->sblk);
}

bool lsm_check_full(block_mgr_t *self, bm_superblock_t *sblk)
{
    if (sblk != NULL)
        return sblk->wp_offt >= SBLK_END;
}

ppa_t lsm_get_page_num(block_mgr_t *self, bm_superblock_t *sblk)
{
    if (!lsm_check_full(self, sblk))
    {
        ppa_t res = SBLK_OFFT2PPA(sblk, sblk->wp_offt++);
        return res;
    }
}

bm_superblock_t *lsm_get_active_sblk(block_mgr_t *self, int type, bool isreserve)
{
    bm_env_t *env = self->env;
    bm_sblk_master_t *master = &env->sblk_master[type];
    if (master->active_sblk == -1)
    {
        for (int i = master->min_sblk_idx; i <= master->max_sblk_idx; i++)
        {
            if (i != master->reserve_sblk && !self->check_full(self, &env->sblk[i]))
            {
                master->active_sblk = i;
                break;
            }
        }
    }
    else
    {
        uint32_t start_sblk = master->active_sblk;
        master->active_sblk = -1;
        for (int i = start_sblk; i <= master->max_sblk_idx; i++)
        {
            if (i != master->reserve_sblk && !self->check_full(self, &env->sblk[i]))
            {
                master->active_sblk = i;
                break;
            }
        }
    }
    if (master->active_sblk == -1)
    {
        abort();
        return NULL;
    }
    if (env->sblk[master->active_sblk].wp_offt == 0)
    {
        master->free_sblk_cnt--;
    }
    return &env->sblk[master->active_sblk];
}

bool lsm_isgc_needed(block_mgr_t *self, int type)
{
    bm_env_t *env = self->env;
    bm_sblk_master_t *master = &env->sblk_master[type];

    if (master->free_sblk_cnt < 1 && type == MAP)
    {
        return true;
    }
    else if (type == DATA && master->free_sblk_cnt <= (int)(master->max_sblk_idx * OP / 100))
    {
        return true;
    }
    return false;
}

bm_superblock_t *lsm_get_gc_target(block_mgr_t *self, int type)
{
    bm_env_t *env = self->env;
    bm_sblk_master_t *master = &env->sblk_master[type];
    return &env->sblk[master->min_sblk_idx];
}
