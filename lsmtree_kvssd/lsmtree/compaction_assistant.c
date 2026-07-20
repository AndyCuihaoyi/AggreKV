#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include "compaction.h"
#include "../lsm_tools/latency_manager.h"
#include "lsm_pm.h"
#include "../lsmtree/lsm_utils.h"
extern lsmtree LSM;
compM compactor;
volatile int memcpy_cnt;
volatile int epc_check = 0;
extern volatile int comp_target_get_cnt, gc_target_get_cnt;
pthread_mutex_t compaction_wait;
pthread_mutex_t compaction_flush_wait;
pthread_mutex_t compaction_req_lock;
pthread_cond_t compaction_req_cond;
bool compaction_init()
{
    // CTHREAD = 1 压实线程数量
    compactor.processors = (compP *)malloc(sizeof(compP) * CTHREAD);
    memset(compactor.processors, 0, sizeof(compP) * CTHREAD);

    pthread_mutex_init(&compaction_req_lock, NULL);
    pthread_cond_init(&compaction_req_cond, NULL);

    for (int i = 0; i < CTHREAD; i++)
    {
        compactor.processors[i].master = &compactor;
        pthread_mutex_init(&compactor.processors[i].flag, NULL);
        pthread_mutex_lock(&compactor.processors[i].flag);
        q_init(&compactor.processors[i].q, CQSIZE);
        pthread_create(&compactor.processors[i].t_id, NULL, compaction_main, NULL);
    }
    compactor.stopflag = false;
    pthread_mutex_init(&compaction_wait, NULL);
    pthread_mutex_init(&compaction_flush_wait, NULL);
    pthread_mutex_lock(&compaction_flush_wait);

    compactor.pt_leveling = partial_leveling;
    return true;
}

bool compaction_check(request *const req, bool *force)
{
    bool res = false;
    if (LSM.memtable->size < FLUSH_CHECK_NUM)
    {
        *force = false;
        return res;
    }
    compR *compReq;
    bool last;
    uint32_t avg_cnt;
    skiplist *t = NULL, *t2 = NULL;
    do
    {
        last = 0;
        if (t2 != NULL)
        {
            t = t2;
        }
        else
        {
            // t是分割后新的skiplist，avg_cnt是分割除去的元素数目
            t = skiplist_cutting_header(LSM.memtable, &avg_cnt);
            if (t == LSM.memtable)
                break;
            // 重新计算header中key的平均数量
            res = true;
            LSM.llp.avg_keynum_inheader = (LSM.llp.avg_keynum_inheader * LSM.llp.cut_header_cnt + avg_cnt) / (LSM.llp.cut_header_cnt + 1);
            LSM.llp.cut_header_cnt++;
        }
        // 如果分割后不需要再分割，t2会满足t2 == LSM.memtable循环结束，否则再分割一个新的skiplist
        t2 = skiplist_cutting_header(LSM.memtable, &avg_cnt);

        if (t2 == LSM.memtable) // 满足t2 == LSM.memtable循环结束,否则不断循环分割skiplist
        {
            LSM.llp.avg_keynum_inheader = (LSM.llp.avg_keynum_inheader * LSM.llp.cut_header_cnt + avg_cnt) / (LSM.llp.cut_header_cnt + 1);
            LSM.llp.cut_header_cnt++;
            last = 1;
        }
        compReq = (compR *)malloc(sizeof(compR));
        compReq->fromL = -1;
        compReq->last = last;
        compReq->temptable = t;
        compReq->parent = req;
        req->is_compacting=true;
        compaction_assign(compReq);
    } while (!last);
    return res;
}

void compaction_assign(compR *req)
{
    bool insert_res = false;
    while (1)
    {
        for (int i = 0; i < CTHREAD; i++)
        {
            compP *proc = &compactor.processors[i];
            pthread_mutex_lock(&compaction_req_lock);
            if (proc->q->size == 0) // empty
            {
                if (q_enqueue((void *)req, proc->q))
                {
                    insert_res = true;
                }
                else
                {
                    printf("compP req queue insert ERROR!\n"); // 如果size=0应该一定可以插入队列
                }
            }
            else
            {
                if (q_enqueue((void *)req, proc->q))
                {
                    // printf("comp req cnt: %d \n", proc->q->size);
                    insert_res = true;
                }
                else
                {
                    insert_res = false;
                }
            }
            if (insert_res)
            {
                pthread_cond_signal(&compaction_req_cond); // 唤醒线程
                pthread_mutex_unlock(&compaction_req_lock);
                break;
            }
            else
            {
                pthread_mutex_unlock(&compaction_req_lock);
            }
        }
        if (insert_res)
        {
            break;
        }
    }
}

void *compaction_main(void *input)
{
    LSM.result_padding = 500;
    void *_req;
    compR *compReq;
    compP *_this = NULL;
    // static int ccnt=0;
    char thread_name[128] = {0};
    pthread_t current_thread = pthread_self();
    sprintf(thread_name, "%s", "compaction_thread");
    // pthread_setname_np(pthread_self(), thread_name);

    for (int i = 0; i < CTHREAD; i++)
    {
        if (pthread_self() == compactor.processors[i].t_id)
        {
            _this = &compactor.processors[i];
        }
    }
    while (1)
    {
        pthread_mutex_lock(&compaction_req_lock);
        if (_this->q->size == 0)
        {
            pthread_cond_wait(&compaction_req_cond, &compaction_req_lock);
        }
        _req = q_pick(_this->q);
        pthread_mutex_unlock(&compaction_req_lock);
        u_int32_t total_lat = 0;
        if (compactor.stopflag)
            break;

        compReq = (compR *)_req;
        leveling_node lnode;
        // if from skiplist
        if (compReq->fromL == -1)
        {
            lnode.mem = compReq->temptable;
            total_lat += compaction_data_write(&lnode); // 对lnose中 mem的snode进行value_set分配，根据分配的value_set执行数据写入
            total_lat += leveling(NULL, LSM.disk[0], &lnode, &LSM.level_lock[0]);
            compReq->parent->end_type = SKIP_FLUSH;
        }
        if (LSM.LEVELN != 1)
        {
            total_lat += compaction_cascading();
            if (total_lat > 0)
                compReq->parent->end_type = CASCADE_FLUSH;
        }
        free(lnode.start.key);
        free(lnode.end.key);
        // compaction之后至少有一层为0，不用检查否则打印检查信息
        bool check = true;
        for (int i = 0; i < LSM.LEVELN; i++)
        {
            if (LSM.disk[i]->n_num == 0)
            {
                check = false;
                break;
            }
        }
        if (check)
        {
            //printf("write_cnt %d\n", LSM.lrr.data_write_cnt);
            //LSM.lop->print_level_summary();
        }
        compReq->parent->etime += total_lat;
        compReq->parent->is_compacting=false;
        q_dequeue(_this->q);
    }
}

uint32_t compaction_empty_level(level **from, leveling_node *lnode, level **des)
{
    u_int32_t total_lat = 0;
    u_int32_t cxl_access = 0;
    // 如果从memtable进行
    if (!(*from))
    {
        run_t *entry = LSM.lop->make_run(lnode->start, lnode->end, -1);
        free(entry->key.key);
        free(entry->end.key);
        // entry->cpt_data初始化htable(metadata)
        LSM.lop->mem_cvt2table(lnode->mem, entry);
        // 如果在top K层
        if ((*des)->idx < LSM.LEVELCACHING)
        {
            // no copy直接将level_caching_data指向htable的keysets
            entry->iscached = 1;
            entry->level_caching_data = (char *)entry->cpt_data->sets;
            entry->cpt_data->sets = NULL; // 置空
            // htable_free(entry->cpt_data); // 释放

            // 将entry插入目标层
            LSM.lop->insert((*des), entry);
            LSM.lop->release_run(entry);
            // cxl cnt
            if ((*des)->isCXL)
            {
                char *ptr = (char *)entry->cpt_data->sets;
                uint16_t *bitmap = (uint16_t *)ptr;
                cxl_access = bitmap[0];
            }
        }
        // 不在top K层
        else
        {
            // 将entry插入目标层，将htable转化为HEADERW请求写入
            entry->iscached = 0;
            entry->level_caching_data = (char *)entry->cpt_data->sets;
            if (entry->cpt_data == NULL)
                abort();
            total_lat += compaction_htable_write_insert((*des), entry);
        }
        free(entry);
    }
    else
    {
        // 如果从top K到下层，遍历源层的每个run
        if ((*des)->idx >= LSM.LEVELCACHING && (*from)->idx < LSM.LEVELCACHING)
        {
            lev_iter *iter = LSM.lop->get_iter((*from), (*from)->start, (*from)->end);
            run_t *now;
            while ((now = LSM.lop->iter_nxt(iter)))
            {
                if ((*from)->isCXL)
                    cxl_access++;
                uint32_t ppa = lsm_get_page(LSM.bm, MAP);
                now->pbn = ppa;
                // level_caching_data to cpt_data
                now->cpt_data = htable_assign(now->level_caching_data);
                // 将每一个run的htable写入flash
                total_lat += compaction_htable_write(ppa, now->cpt_data, now->key);
                now->cache_data = 0;
            }
        }
        // 由于des是empty层，直接将from的参数拷贝过来
        LSM.lop->lev_copy(*des, *from);
    }
    total_lat += cxl_access * CXL_DIR_LAT;
    return total_lat;
}

void compaction_sub_pre()
{
    pthread_mutex_lock(&compaction_wait);
    memcpy_cnt = 0;
}

// htable读预处理，如果有缓存的metadata，memcpy_cnt++返回true，否则分配htable并返回false
bool htable_read_preproc(run_t *r)
{
    bool res = false;
    if (r->level_caching_data == NULL && r->cpt_data == NULL)
    {
        LSM.lop->print_level_summary();
        abort();
    }
    else
    {
        if (!keyset_check(r->level_caching_data))
            abort();
        // htable_print(r->level_caching_data);
    }
    if (r->iscached == 1)
    {
        memcpy_cnt++;
        return true;
    }
    r->cpt_data = htable_assign(NULL);
    r->cpt_data->iscached = 0;
    if (!r->iscompactioning)
        r->iscompactioning = true;
    return res;
}

void compaction_sub_wait()
{
    if (epc_check == comp_target_get_cnt + memcpy_cnt)
        pthread_mutex_unlock(&compaction_wait);
    memcpy_cnt = 0;
    comp_target_get_cnt = 0;
    epc_check = 0;
}

// 合并metadata 合并skiplist
u_int64_t compaction_subprocessing(struct skiplist *top, struct run **src, struct run **org, struct level *des)
{
    compaction_sub_wait();
    u_int64_t total_lat = 0;
    // 合并上下层的所有run的metapage合并(混合归并排序放在一个数组中),将原有的free
    LSM.lop->merger(top, src, org, des);
    KEYT key, end;
    run_t *target = NULL;
    // 把切割合并后的数组切割成一个个run插入到目标层，如果插入top K直接插，否则写入flash再插入
    while ((target = LSM.lop->cutter(top, des, &key, &end)))
    {
        if (des->idx < LSM.LEVELCACHING)
        {
            target->iscached = 1;
            LSM.lop->insert(des, target);
            LSM.lop->release_run(target);
        }
        else
        {
            target->iscached = 0;
            if (target->cpt_data == NULL)
                abort();
            total_lat += compaction_htable_write_insert(des, target);
        }
        free(target);
    }
    return total_lat;
}

void htable_read_postproc(run_t *r)
{
    if (r->iscompactioning != INVBYGC && r->iscompactioning != SEQCOMP)
    {
        if (r->pbn != UINT32_MAX)
        {
            // invalidate_PPA(HEADER, r->pbn);
        }
        else
        {
            // the run belong to levelcaching lev
        }
    }
    if (r->level_caching_data)
    {
    }
    else
    {
        if (r->pbn == UINT32_MAX)
        {
            LSM.lop->release_run(r);
            free(r);
        }
    }
}

void compaction_sub_post()
{
    pthread_mutex_unlock(&compaction_wait);
}

u_int64_t compaction_cascading()
{
    int start_level = 0, des_level = -1;
    u_int64_t total_lat = 0;
    while (1)
    {
        // 进行级联压缩
        if (unlikely(LSM.lop->full_check(LSM.disk[start_level])))
        {
            des_level = (start_level == LSM.LEVELN ? start_level : start_level + 1);
            if (des_level == LSM.LEVELN)
                break;
            total_lat += leveling(LSM.disk[start_level], LSM.disk[des_level], NULL, &LSM.level_lock[des_level]);
            LSM.disk[start_level]->iscompactioning = false;
            start_level++;
        }
        else
        {
            break;
        }
    }
    return total_lat;
}
