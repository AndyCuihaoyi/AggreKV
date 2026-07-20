#include <pthread.h>
#include <math.h>
#include "compaction.h"
#include "../lower/ssd.h"
#include "../lsmtree/lsmtree.h"
#include "../lsm_tools/latency_manager.h"
extern compM compactor;
extern lsmtree LSM;
extern KEYT key_min, key_max;
extern volatile int epc_check;
u_int64_t leveling(level *from, level *to, leveling_node *l_node, pthread_mutex_t *lock)
{
    u_int64_t total_lat = 0;

    level *target_origin = to;
    // compacted target
    level *target = lsm_level_resizing(to, from);
    run_t *entry = NULL;
    uint32_t up_num = 0; // run num in upper level
    if (from)
    {
        up_num = LSM.lop->get_number_runs(from);
    }
    else // from skiplist run will be 1
    {
        up_num = 1;
    }
    uint32_t total_number = to->n_num + up_num + 1;
    if (LSM.LEVELN == 1)
    {
        run_t *temp = LSM.lop->make_run(l_node->start, l_node->end, -1);
        htable *write_target;
        write_target = LSM.lop->mem_cvt2table(l_node->mem, temp);
        lev_iter *iter = LSM.lop->get_iter(to, to->start, to->end);
        run_t *now;
        while ((now = LSM.lop->iter_nxt(iter)))
        {
            LSM.lop->insert(target, now);
        }
        temp->iscached = 0;
        temp->level_caching_data = (char *)write_target;
        if (temp->cpt_data == NULL)
            abort();
        total_lat += compaction_htable_write_insert(target, temp);
        goto last;
    }
    if (target->idx < LSM.LEVELCACHING)
    {
        if (to->n_num == 0)
        {
            /*
                处理三种情况：
                    1.从memtable向top K空层下刷
                    2.从memtable向top K以下空层下刷
                    3.从top K向top K下的空层下刷
             */
            total_lat += compaction_empty_level(&from, l_node, &target);
            goto last;
        }
        // 合并下刷
        total_lat += partial_leveling(target, target_origin, l_node, from);
    }
    else
    {
        // 目的层是空，向空层下刷后goto last
        if (to->n_num == 0)
        {
            total_lat += compaction_empty_level(&from, l_node, &target);
            goto last;
        }
        // compaction计数++(向flash下刷)
        LSM.lrr.compaction_cnt++;
        if (to->idx == LSM.LEVELN - 1)
            LSM.lrr.last_compaction_cnt++;

        if (from == NULL && target->idx >= LSM.LEVELCACHING)
        {
            uint32_t ppa = lsm_get_page(LSM.bm, MAP);
            entry = LSM.lop->make_run(l_node->start, l_node->end, ppa);
            free(entry->key.key);
            free(entry->end.key);
            LSM.lop->mem_cvt2table(l_node->mem, entry);
            total_lat += compaction_htable_write(ppa, entry->cpt_data, entry->key);
            l_node->entry = entry;
        }
        // 这里决定了下刷方式，在初始化时设定，默认partial_leveling和上面一样
        total_lat += compactor.pt_leveling(target, target_origin, l_node, from);
    }

last:
    if (entry)
        free(entry);
    uint32_t res = level_change(from, to, target, lock);
    // printf("ending\n");
    LSM.c_level = NULL;
    // LSM.lop->print_level_summary();
    if (target->idx == LSM.LEVELN - 1)
    {
        printf("last level %d/%d (n:f)\n", target->n_num, target->m_num);
    }
    return total_lat;
}

/*
    两种情况:
    1. memtable下刷，将skiplist转为metadata，获取目标层metadata合并下刷
    2. 其他情况下刷，获取源层和目标层全部metadata合并下刷

    参数：
    level *t  目标层
    level *origin 原目标层
    leveling_node lnode
    level *upper 源层
 */
u_int64_t partial_leveling(level *t, level *origin, leveling_node *lnode, level *upper)
{
    KEYT start = key_min;
    KEYT end = key_max;
    run_t **target_s = NULL;
    run_t **data = NULL;
    skiplist *skip = lnode ? lnode->mem : skiplist_init();
    compaction_sub_pre();

    u_int32_t cxl_access = 0;
    u_int64_t total_lat = 0;
    // 没有源层
    if (!upper)
    {
        // 将origin中start key上界的run拷贝到target_s中(在此情况下是所有)
        LSM.lop->range_find_compaction(origin, start, end, &target_s);

        // 遍历target_s中的run,获取全部run的metadata(无论在flash还是mem),subprocess合并所有metadata和skiplist
        for (int j = 0; target_s[j] != NULL; j++)
        {
            // 预处理，没有cpt_data后面没办法读
            if (!htable_read_preproc(target_s[j]))
            {
                // 没有metadata缓存，进行htable读
                total_lat += compaction_htable_read(target_s[j], (PTR *)&target_s[j]->cpt_data);
            }
            epc_check++; // 有metadata缓存，计数器++
            if (origin->isCXL)
                cxl_access++;
        }
        total_lat += cxl_access * CXL_DIR_LAT;
        total_lat += compaction_subprocessing(skip, NULL, target_s, t);

        // 对于cpt_data指向htable的全部free
        for (int j = 0; target_s[j] != NULL; j++)
        {
            htable_read_postproc(target_s[j]);
        }
        free(target_s);
    }
    else
    {
        int src_num, des_num; // for stream compaction
                              // 获得原目标层数目，将原目标层run放在target_s中

        des_num = LSM.lop->range_find_compaction(origin, start, end, &target_s); // for stream compaction
        // 分情况将源层的run放在data中
        if (upper->idx < LSM.LEVELCACHING)
        {
            src_num = LSM.lop->cache_comp_formatting(upper, &data, upper->idx < LSM.LEVELCACHING);
        }
        else
        {
            src_num = LSM.lop->range_find_compaction(upper, start, end, &data);
        }
        if (src_num && des_num == 0)
        {
            printf("can't be\n");
            abort();
        }
        if (origin->isCXL)
            cxl_access += src_num;
        if (upper->isCXL)
            cxl_access += des_num;
        total_lat += cxl_access * CXL_DIR_LAT;
        // 获取目标层全部metadata
        for (int i = 0; target_s[i] != NULL; i++)
        {
            run_t *temp = target_s[i];
            if (temp->iscompactioning == SEQCOMP)
            {
                continue;
            }
            if (!htable_read_preproc(temp))
            {
                total_lat += compaction_htable_read(temp, (PTR *)&temp->cpt_data);
            }
            epc_check++;
        }

        // 如果源层在top K则直接去合并下刷，否则先获取源层所有metadata
        if (upper->idx < LSM.LEVELCACHING)
        {
            goto skip;
        }
        for (int i = 0; data[i] != NULL; i++)
        {
            run_t *temp = data[i];
            if (!htable_read_preproc(temp))
            {
                total_lat += compaction_htable_read(temp, (PTR *)&temp->cpt_data);
            }
            epc_check++;
        }
    skip:
        // 合并下刷
        total_lat += compaction_subprocessing(NULL, data, target_s, t);

        // free临时htable
        for (int i = 0; data[i] != NULL; i++)
        {
            run_t *temp = data[i];
            htable_read_postproc(temp);
        }

        for (int i = 0; target_s[i] != NULL; i++)
        {
            run_t *temp = target_s[i];
            htable_read_postproc(temp);
        }
        free(data);
        free(target_s);
    }
    // 解锁
    compaction_sub_post();
    if (!lnode)
        skiplist_free(skip);
    //	LSM.lop->print_level_summary();
    return total_lat;
}

u_int64_t compaction_data_write(leveling_node *lnode)
{
    value_set **data_sets = skiplist_make_valueset(lnode->mem, LSM.disk[0], &lnode->start, &lnode->end);
    // write data for each value_set
    u_int64_t total_lat = 0;
    int i = 0;
    for (i = 0; data_sets[i] != NULL; i++)
    {
        total_lat+= LSM.algo->li->write((data_sets[i]->ppa), PAGESIZE, 0);
        LSM.lrr.back_end_write++;
    }
    // total_lat+=i*500000;
    free(data_sets);
    return total_lat;
}

u_int64_t compaction_htable_write_insert(level *target, run_t *entry)
{
    ppa_t ppa = lsm_get_page(LSM.bm, MAP);
    entry->pbn = ppa;
    // 将entry插入目标层
    LSM.lop->insert(target, entry);
    // 将htable转化为HEADERW请求，写入
    if (entry->cpt_data == NULL)
        abort();
    u_int64_t w_lat = compaction_htable_write(entry->pbn, entry->cpt_data, entry->key);
    LSM.lop->release_run(entry);
    return w_lat;
}

u_int64_t compaction_htable_read(run_t *ent, PTR *value)
{
    u_int64_t r_lat = 0;
    value = &ent->level_caching_data;
    r_lat = LSM.algo->li->read((ent->pbn), PAGESIZE, 0);
    return r_lat;
}

u_int64_t compaction_htable_write(ppa_t ppa, htable *input, KEYT lpa)
{
    u_int64_t w_lat = 0;
    if (input->origin)
    {
        w_lat = LSM.algo->li->write((ppa), PAGESIZE, 0);
    }
    else
    {
        input->origin = inf_get_valueset(NULL, PAGESIZE);
        w_lat = LSM.algo->li->write((ppa), PAGESIZE, 0);
    }
    return w_lat;
}

level *lsm_level_resizing(level *target, level *src)
{
    // not completed....
    return LSM.lop->init(ceil(target->m_num), target->idx, false);
}

uint32_t level_change(level *from, level *to, level *target, pthread_mutex_t *lock)
{
    level **src_ptr = NULL, **des_ptr = NULL;
    int from_idx = 0;
    // free original level
    if (from != NULL)
    {
        from_idx = from->idx;
        pthread_mutex_lock(&LSM.level_lock[from_idx]);
        src_ptr = &LSM.disk[from->idx];
        *(src_ptr) = LSM.lop->init(from->m_num, from->idx, from->istier);
        pthread_mutex_unlock(&LSM.level_lock[from_idx]);
        LSM.lop->release(from);
    }
    // point to new level
    pthread_mutex_lock(lock);
    des_ptr = &LSM.disk[to->idx];
    target->iscompactioning = to->iscompactioning;
    (*des_ptr) = target;
    pthread_mutex_unlock(lock);
    LSM.lop->release(to);
    return 1;
}
