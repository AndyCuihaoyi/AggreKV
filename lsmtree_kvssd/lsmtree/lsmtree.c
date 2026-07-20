#include "lsmtree.h"
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lsm_settings.h"
#include "lsm_bm.h"
#include "lsm_pm.h"
#include "lsm_utils.h"
#include "compaction.h"
#include "../lsm_tools/rte_ring/rte_ring.h"
#include "../lsm_tools/container.h"
#include "../lsm_tools/latency_manager.h"
#include "../array/array.h"
#include "../lower/ssd.h"
volatile int comp_target_get_cnt = 0, gc_target_get_cnt;
extern level_ops a_ops;
extern block_mgr_t bm;
extern lower_info ssd_li;
extern page_t page;
lsmtree LSM;

algorithm lsm_algo =
    {
        .read = lsm_get,
        .write = lsm_set,
        .create = lsm_create,
        .destroy = lsm_destroy,
        .li = &ssd_li};

void level_params_print()
{
    printf("| -------- level_log Srart\n\n");
    fprintf(stderr, "SHOWINGSIZE(GB) :%lu \n", SHOWINGSIZE / G);
    fprintf(stderr, "LEVELN:%d (LEVELCACHING(%d))\n", LSM.LEVELN, LSM.LEVELCACHING);
    // level 数目,层级大小因数
    printf("| sizefactor:%lf last:%lf\n", LSM.llp.size_factor, LSM.llp.last_size_factor);
    // all level data(key-value)
    printf("| all level size: %lf(GB)", (double)LSM.all_header_num * LSM.lsp.ONESEGMENT / G);
    // 展示给用户的空间大小 （出去OP）
    printf(" target size: %lf(GB)\n", (double)SHOWINGSIZE / G);                                         // showingSize=totalsize/100*op OP=70 default
    uint32_t TOTALHEADER = (TOTALSIZE / LSM.lsp.ONESEGMENT) + (TOTALSIZE % LSM.lsp.ONESEGMENT ? 1 : 0); // 全填满所需段的总数，包括OP
    printf("| level list size: %u MB\n", (TOTALHEADER * (DEFKEYLENGTH + 4)) / 1024 / 1024);             // 从B转化为MB打印
    printf("| all level header size: %lu(MB), except last header: %lu(MB)\n", LSM.all_header_num * PAGESIZE / M, (LSM.all_header_num - LSM.disk[LSM.LEVELN - 1]->m_num) * PAGESIZE / M);
    printf("| WRITE WAF:%f\n", (float)(LSM.llp.size_factor * (LSM.LEVELN - 1 - LSM.LEVELCACHING) + LSM.llp.last_size_factor) / LSM.lsp.KEYNUM + 1);
    printf("| used memory :%lu MB\n", (LSM.lsp.total_memory - LSM.lsp.remain_memory) / (M));
    printf("| level pinning :%luMB(%lu page)%.2f(%%)\n", LSM.lsp.pin_memory / M, LSM.lsp.pin_memory / PAGESIZE, (float)LSM.lsp.pin_memory / LSM.lsp.total_memory * 100);
    printf("| entry cache :%luMB(%lu page)%.2f(%%)\n", LSM.lsp.cache_memory / M, LSM.lsp.cache_memory / PAGESIZE, (float)LSM.lsp.cache_memory / LSM.lsp.total_memory * 100);
    // printf("\n ---------- %lu:%d (all_entry : total)\n\n", LSM.all_header_num, MAPPART_SEGS * _PPS);
    printf("\n| -------- level_log End\n");
}

uint32_t lsm_create(algorithm *lsm_algo, lower_info *li)
{
    // for max and min  key
    key_max.key = (char *)malloc(sizeof(char) * MAXKEYLENGTH);
    key_max.len = MAXKEYLENGTH;
    memset(key_max.key, -1, sizeof(char) * MAXKEYLENGTH);

    key_min.key = (char *)malloc(sizeof(char) * MAXKEYLENGTH);
    key_min.len = MAXKEYLENGTH;
    memset(key_min.key, 0, sizeof(char) * MAXKEYLENGTH);

    // for params
    LSM.LEVELN = DEF_LEVEL;
    LSM.LEVELCACHING = DEF_TOP_K_LEVEL;
    lsm_setup_params();
    // for record
    LSM.lrr.back_end_write = 0;
    LSM.lrr.data_read_cnt = 0;
    LSM.lrr.data_write_cnt = 0;
    LSM.lrr.gc_cnt = 0;
    LSM.lrr.compaction_cnt = 0;
    LSM.lrr.last_compaction_cnt = 0;
    LSM.llp.avg_keynum_inheader = 0;
    LSM.llp.cut_header_cnt = 0;

    // for block and page manager
    bm.create(&bm);
    lsm_page_init(&bm);
    LSM.bm = &bm;
    LSM.pm = &page;

    // for ssd lower
    lsm_algo->li->create(lsm_algo->li);

    //  for skiplist
    LSM.memtable = skiplist_init();
    // for level
    LSM.lop = &a_ops;
    LSM.level_lock = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t) * LSM.LEVELN);
    for (int i = 0; i < LSM.LEVELN; i++)
    {
        pthread_mutex_init(&LSM.level_lock[i], NULL);
    }

    float m_num = 1;
    LSM.all_header_num = 0;
    LSM.disk = (level **)malloc(sizeof(level *) * LSM.LEVELN);
    for (int i = 0; i < LSM.LEVELN; i++)
    { // for lsmtree -1 level
        // m_size指的是run_t的数目
        LSM.disk[i] = LSM.lop->init(ceil(m_num * LSM.llp.size_factor), i, false);
        if (i < DEF_TOP_K_LEVEL && i >= (DEF_TOP_K_LEVEL - DEF_CXL_LEVEL))
        {
            LSM.disk[i]->isCXL = true;
        }
        else
        {
            LSM.disk[i]->isCXL = false;
        }
        LSM.all_header_num += LSM.disk[i]->m_num;
        m_num *= LSM.llp.size_factor;
    }
    level_params_print();

    lsm_algo->req_q = ring_create(RING_TYPE_MP_SC, MAX_INF_REQS);
    lsm_algo->finish_q = ring_create(RING_TYPE_MP_MC, MAX_INF_REQS);
    compaction_init();
}
void lsm_destroy(algorithm *lsm_algo, lower_info *li)
{
    printf("lsm_destroy\n");
}
uint32_t lsm_set(algorithm *lsm_algo, request *const req)
{
    static bool force = 0;
    LSM.lrr.data_write_cnt++;
    if (!compaction_check(req, &force))
    {
        req->end_type = NO_COMPACTION;
    }
    else
    {
        req->is_compacting=true;
        while(req->is_compacting==true)
        {
            usleep(1000);
        }
    }
    skiplist_insert(LSM.memtable, req, false);
    if (LSM.memtable->size == FLUSH_CHECK_NUM)
    {
        force = 1;
        return 1;
    }

    /* Complete the request: set etime to absolute completion time */
    req->etime = clock_get_ns();
    ring_enqueue(LSM.algo->finish_q, (void *)&req, 1);
    return 0;
}

extern int read_type_cnt[4];
uint32_t lsm_get(algorithm *lsm_algo, request *const req)
{
    int level;
    int run;
    int round;
    int res;
    htable mapinfo;
    run_t *entry;
    keyset *found = NULL;
    uint8_t result = 0;

    int *temp_data;
    rparams *rp;
    if (req->params == NULL)
    {
        /*skiplist and templist*/
        res = __lsm_get_sub(req, NULL, NULL, LSM.memtable, 0);
        if (unlikely(res == SKIP_FOUND))
        {
            goto end_req;
        }
        // Check temptable
        pthread_mutex_lock(&LSM.templock);
        // res = __lsm_get_sub(req, NULL, NULL, LSM.temptable, 0);
        pthread_mutex_unlock(&LSM.templock);

        // found
        if (unlikely(res == SKIP_FOUND))
        {
            goto end_req;
        }
    end_req:
        {
            req->end_type = SKIP_FOUND;
            read_type_cnt[0]++;
            req->etime = clock_get_ns();
            ring_enqueue(LSM.algo->finish_q, (void *)&req, 1);
            return res;
        }

        // not found - allocate params for level search
        rp = (rparams *)malloc(sizeof(rparams));
        req->params = (void *)rp;
        rp->entry = NULL;
        entry = NULL;

        temp_data = rp->datas;
        temp_data[0] = level = 0;
        temp_data[1] = run = 0;
        temp_data[2] = round = 0;
        temp_data[3] = 0;
    }
    else
    {
        // reuse existing params from previous partial read
        rp = (rparams *)req->params;
        temp_data = rp->datas;
        level = temp_data[0];
        run = temp_data[1];
        round = temp_data[2];
        entry = rp->entry;
    }
retry:
    int cxl_access;
    result = lsm_find_run(req->key, &entry, entry, &found, &level, &run, &cxl_access);
    req->etime += cxl_access * CXL_DIR_LAT;
    if (temp_data[3] == 1)
        temp_data[3] = 0;
    switch (result)
    {
    case CACHING:
        if (found)
        {
            read_type_cnt[1]++;
            req->etime += LSM.algo->li->read((found->ppa), PAGESIZE, 0);
        }
        else
        {
            level++;
            goto retry;
        }
        res = CACHING;
        break;
    case FOUND:
        temp_data[0] = level;
        temp_data[1] = run;
        rp->entry = entry;
        temp_data[2] = ++round;
        entry->isflying = 1;
        req->ppa = entry->pbn;
        // read metadata in flash
        req->etime += LSM.algo->li->read((entry->pbn), PAGESIZE, 0);
        keyset *find = LSM.lop->find_keyset(entry->level_caching_data, req->key);
        // read value
        req->etime += LSM.algo->li->read((find->ppa), PAGESIZE, 0);
        read_type_cnt[2]++;
        LSM.lrr.data_read_cnt++;
        res = FLYING;
        break;
    case NOTFOUND:
        read_type_cnt[3]++;
        res = NOTFOUND;
        break;
    default:
        res = NOTFOUND;
        break;
    }
    req->etime = req->stime + req->etime;
    ring_enqueue(LSM.algo->finish_q, (void *)&req, 1);
    return 0;
}

int __lsm_get_sub(request *req, run_t *entry, keyset *table, skiplist *list, int idx)
{
    /*
        取出/查找失败返回0
        查找 memtable或temp_table的snode value_set不为NULL 返回2
        snode value_set为NULL转换请求类型，res=4;cache 命中res=4;都会将请求转化为algo_req
        转化请求后，检查有效性，使用LSM.li->read或LSM.li->read_hw函数异步读取数据，并将请求传递给LSM树的读取接口
        如果ppa为最大整型，释放请求并返回5
    */
    int res = 0;
    if (!entry && !table && !list && idx != LSM.LEVELN - 1)
    {
        return 0;
    }
    uint32_t ppa;
    snode *target_node;
    keyset *target_set;
    // checking skiplist and templist
    if (list)
    { // skiplist check for memtable and temp_table;
        target_node = skiplist_find(list, req->key);
        if (!target_node)
            return NOT_FOUND;   // 节点不在memtable或temptable中，return 0
        if (target_node->value) // value_set不为空，返回2;否则将res置为4，转换请求类型，取出ppa
        {
            return SKIP_FOUND;
        }
        else
        {
            // 一个转换函数，将普通请求转化为算法请求，类型为data read(DATAR)
            // lsm_req = lsm_get_req_factory(req, DATAR, 0);
            req->value->ppa = target_node->ppa;
            ppa = target_node->ppa;
            req->value->ppa = ppa;
            req->etime += LSM.algo->li->read((req->value->ppa), PAGESIZE, 0);
            return SKIP_FOUND;
        }
    }
    return res;
}

uint8_t lsm_find_run(KEYT key, run_t **entry, run_t *up_entry, keyset **found, int *level, int *run, int *cxl_access)
{
    run_t *entries = NULL;
    if (*run)
        (*level)++;
    for (int i = *level; i < LSM.LEVELN; i++)
    {
        entries = LSM.lop->find_run(LSM.disk[i], key);
        if (!entries)
        {
            // 没有找到对应的run_t跳到下一层
            continue;
        }
        if (i < LSM.LEVELCACHING)
        {
            // 如果在top-K层，返回读类型为CACHING;
            keyset *find = LSM.lop->find_keyset(entries->level_caching_data, key);
            if (LSM.disk[i]->isCXL)
            {
                double log2_n = log2(LSM.disk[i]->n_num); // 换底公式
                *cxl_access = ceil(log2_n);               // 向上取整
                uint16_t *bitmap = (uint16_t *)entries->level_caching_data;
                log2_n = log2(bitmap[0]);
                *cxl_access += ceil(log2_n);
            }
            if (find)
            {
                *found = find;
                if (level)
                    *level = i;
                return CACHING;
            }
        }
        else
        {
            if (level)
                *level = i;
            if (run)
                *run = 0;
            *entry = entries;
            return FOUND;
        }
        if (run)
            *run = 0;
        continue;
    }
    return NOTFOUND; // 所有bf都判断没有才返回not found
}

htable *htable_assign(char *cpy_src)
{
    htable *res = (htable *)malloc(sizeof(htable));
    value_set *temp;
    if (cpy_src)
        temp = inf_get_valueset(cpy_src, PAGESIZE);
    else
        temp = inf_get_valueset(NULL, PAGESIZE);
    res->sets = (keyset *)temp->value;
    res->origin = temp;
    return res;
}

void htable_free(htable *input)
{
    free(input->sets);
    free(input);
}

htable *htable_copy(htable *input)
{
    htable *res = (htable *)malloc(sizeof(htable));
    res->sets = (keyset *)malloc(PAGESIZE);
    memcpy(res->sets, input->sets, PAGESIZE);
    res->origin = NULL;
    return res;
}