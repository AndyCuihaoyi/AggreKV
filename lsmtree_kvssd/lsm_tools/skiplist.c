#ifndef SKIPLIST_H__
#define SKIPLIST_H__

#include "skiplist.h"
#include "../lsmtree/lsmtree.h"
#include "../lsmtree/lsm_utils.h"
#include "../lsmtree/lsm_bm.h"
#include "../lsmtree/lsm_pm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
extern KEYT key_max, key_min;
extern lsmtree LSM;
extern bm_env_t bm_env;
skiplist *skiplist_init()
{
    skiplist *point = (skiplist *)malloc(sizeof(skiplist));
    point->level = 1;
    point->header = (snode *)malloc(sizeof(snode));
    point->header->list = (snode **)malloc(sizeof(snode *) * (MAX_L + 1));
    for (int i = 0; i < MAX_L; i++)
        point->header->list[i] = point->header;
    point->all_length = 0;
    point->size = 0;
    // point back
    point->header->back = point->header;
    point->header->key = key_max;
    point->header->value = NULL;
    return point;
}

snode *skiplist_find(skiplist *list, KEYT key)
{
    if (!list)
        return NULL;
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) < 0)
            x = x->list[i];
    }
    if (KEYTEST(x->list[1]->key, key))
        return x->list[1];
    return NULL;
}

snode *skiplist_find_lowerbound(skiplist *list, KEYT key)
{
    if (!list)
        return NULL;
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) < 0)

            x = x->list[i];
    }
    return x->list[1];
}

snode *skiplist_range_search(skiplist *list, KEYT key)
{
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    snode *bf = list->header;
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) <= 0)
        {
            bf = x;
            x = x->list[i];
        }
    }
    bf = x;
    x = x->list[1];
    if (KEYCMP(bf->key, key) <= 0 && KEYCMP(key, x->key) < 0)
    {
        return bf;
    }
    if (KEYCMP(key, list->header->list[1]->key) <= 0)
    {
        return list->header->list[1];
    }
    return NULL;
}

snode *skiplist_strict_range_search(skiplist *list, KEYT key)
{
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    snode *bf = list->header;
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) <= 0)
        {
            bf = x;
            x = x->list[i];
        }
    }

    bf = x;
    x = x->list[1];
    if (KEYCMP(bf->key, key) <= 0 && KEYCMP(key, x->key) < 0)
    {
        return bf;
    }
    else if (KEYCMP(bf->key, key_max) == 0)
    {
        return x;
    }
    return NULL;
}

static int getLevel()
{
    int level = 1;
    int temp = rand();
    while (temp % PROB == 1) // 1/PROB chance level+=1,1/16 level+=2....
    {
        temp = rand();
        level++;
        if (level + 1 >= MAX_L)
            break;
    }
    return level;
}

/*
    used for metadata update after data GC
    ignore existing snode，insert when it doesn't exist
    notice: x->isvalid = deletef; x->value = NULL; no value is insert here
*/
snode *skiplist_insert_wP(skiplist *list, KEYT key, ppa_t ppa, bool deletef)
{
    snode *update[MAX_L + 1];
    snode *x = list->header;

    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) < 0)
            x = x->list[i];
        update[i] = x;
    }
    x = x->list[1];

    if (KEYTEST(key, x->key))
    {
        // ignore new one;
        // invalidate_PPA(DATA, ppa);
        abort();
        return x;
    }
    else
    {
        int level = getLevel();
        if (level > list->level)
        {
            for (int i = list->level + 1; i <= level; i++)
            {
                update[i] = list->header;
            }
            list->level = level;
        }
        x->list = (snode **)malloc(sizeof(snode *) * (level + 1));

        list->all_length += KEYLEN(key);
        x->key = key;
        x->ppa = ppa;
        x->isvalid = deletef;
        x->iscaching_entry = false;
        x->value = NULL;
        for (int i = 1; i <= level; i++)
        {
            x->list[i] = update[i]->list[i];
            update[i]->list[i] = x;
        }

        // new back
        x->back = x->list[1]->back;
        x->list[1]->back = x;
        x->level = level;
        list->size++;
    }
    return x;
}

snode *skiplist_insert(skiplist *list, request *req, bool deletef)
{
    snode *update[MAX_L + 1]; // use for update search path
    snode *x = list->header;
    // search for snode
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, req->key) < 0)
            x = x->list[i];
        update[i] = x; // record path
    }
    x = x->list[1];
    if (req->value != NULL)
    {
        req->value->length = (req->value->length / PIECE) + (req->value->length % PIECE ? 1 : 0);
    }
    // if key exists,update
    if (KEYTEST(req->key, x->key))
    {
        list->data_size -= (x->value->length * PIECE);
        list->data_size += (req->value->length * PIECE);
        // 释放原value空间
        if (x->value)
            inf_free_valueset(&x->value);
        free(req->key.key);
        //  更新value
        x->value = req->value;
        req->value = NULL;
        x->isvalid = deletef;
        if (req->end_type == NO_COMPACTION)
        {
            req->end_type = SKIP_MODIFY;
        }
        return x;
    }
    // no identical key,insert
    else
    {
        int level = getLevel();
        // increace level and update
        if (level > list->level)
        {
            for (int i = list->level + 1; i <= level; i++)
            {
                update[i] = list->header;
            }
            list->level = level;
        }
        x = (snode *)malloc(sizeof(snode));
        x->list = (snode **)malloc(sizeof(snode *) * (level + 1));
        kvssd_cpy_key(&x->key, &req->key);
        // x->key = req->key;
        if (!KEYVALCHECK(x->key))
            abort();
        x->isvalid = deletef;
        x->ppa = UINT_MAX;
        x->value = req->value;
        list->all_length += KEYLEN(x->key);
        x->iscaching_entry = false;
        for (int i = 1; i <= level; i++)
        {
            x->list[i] = update[i]->list[i];
            update[i]->list[i] = x;
        }

        // new back
        x->back = x->list[1]->back;
        x->list[1]->back = x;
        x->level = level;
        list->size++;
        list->data_size += (req->value->length * PIECE);
        req->value = NULL;
        if (req->end_type == NO_COMPACTION)
        {
            req->end_type = SKIP_INSERT;
        }
    }
    return x;
}

skiplist *skiplist_divide(skiplist *in, snode *target)
{
    skiplist *res = skiplist_init();
    // swap
    if (target == in->header)
    {
        skiplist swap;
        memcpy(&swap, in, sizeof(skiplist));
        memcpy(in, res, sizeof(skiplist));
        memcpy(res, &swap, sizeof(skiplist));
        return res;
    }
    uint32_t origin_level = in->level;
    res->level = in->level;

    snode *x = in->header->list[res->level]; // x是最高层的指针
    snode *temp, *temp2;
    uint32_t t_level = (target == in->header ? 1 : target->level); // 目标节点的层数
    // LSM skiplist level到target level
    for (uint32_t i = res->level; i > t_level; i--)
    {
        while (KEYCMP(x->list[i]->key, target->key) < 0) // 找到大于target key的第一个键
            x = x->list[i];
        if (KEYCMP(x->key, target->key) > 0) // 左右都有
        {
            res->level--;
            x = in->header->list[i - 1]; // x到下一层头结点
            continue;                    // 继续
        }
        else if (x->list[i] == in->header) // 直接找到了header表示第一层的snode key没有比target key大的
        {
            in->level--; // 原来的索引层数-1
        }
        // 如果in->level--,temp1此层第一个节点，temp2此层最后一个节点，后面的操作将完整的一层给了res
        // 如果res->level--;跳到下一层，继续判断
        temp = in->header->list[i];
        temp2 = x;

        in->header->list[i] = x->list[i];
        res->header->list[i] = temp;
        temp2->list[i] = res->header;
    }
    // t_level往下，一定以target node为分界线
    // 从target snode左右分割，左边包括target给res，右边留给LSM.mem table
    for (uint32_t i = t_level; i >= 1; i--)
    {
        res->header->list[i] = in->header->list[i];
        in->header->list[i] = target->list[i];
        if (target->list[i] == in->header)
        {
            in->level--;
        }
        target->list[i] = res->header;
    }

    // if ((origin_level != in->level && origin_level != res->level) || (in->level != 0 && (in->header->list[in->level] == in->header || res->header->list[res->level] == res->header)))
    // {
    //     printf("origin_level:%d in->level:%d\n", origin_level, in->level);
    //     printf("res->header->list[res->level] %p, res->header %p\n", res->header->list[res->level], res->header);
    //     printf("in->header->list[in->level] %p, in->header %p\n", in->header->list[in->level], in->header);
    //     printf("skiplist_divide error!\n");
    //     abort();
    // }
    if (in->level == 0)
        in->level = 1;

    return res;
}

void skiplist_print(skiplist *skip)
{
    snode *temp;
    printf("| ------------skiplist print\n");
    printf("snode number: %lu\n", skip->size);
    for_each_sk(temp, skip)
    {
        printf("[lev:%d]%p\t", temp->level, temp);
        for (uint32_t i = 0; i < temp->level; i++)
        {
            printf("[%.*s] ", temp->key.len, temp->key.key);
        }
        printf("\n");
    }

    printf("max level ptr:");
    for (uint32_t i = 1; i <= skip->level; i++)
    {
        printf("%p ", skip->header->list[i]);
    }
    printf("\n");
}

void skiplist_clear(skiplist *list)
{
    snode *now = list->header->list[1];
    snode *next = now->list[1];
    while (now != list->header)
    {
        if (now->value)
        {
            inf_free_valueset(&now->value);
        }
        free(now->key.key);
        free(now->list);
        free(now);
        now = next;
        next = now->list[1];
    }
    list->size = 0;
    list->level = 0;
    for (int i = 0; i < MAX_L; i++)
        list->header->list[i] = list->header;
    list->header->key = key_max;
}

void skiplist_free(skiplist *list)
{
    if (list == NULL)
        return;
    skiplist_clear(list);
    free(list->header->list);
    free(list->header);
    free(list);
    return;
}

int skiplist_delete(skiplist *list, KEYT key)
{
    if (list->size == 0)
        return -1;
    snode *update[MAX_L + 1];
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--)
    {
        while (KEYCMP(x->list[i]->key, key) < 0)
            x = x->list[i];
        update[i] = x;
    }
    x = x->list[1];

    if (KEYCMP(x->key, key) != 0)
        return -2;

    for (int i = x->level; i >= 1; i--)
    {
        update[i]->list[i] = x->list[i];
        if (update[i] == update[i]->list[i])
            list->level--;
    }

    free(x->list);
    free(x);
    list->size--;
    return 0;
}

snode *skiplist_at(skiplist *list, int idx)
{
    snode *header = list->header;
    for (int i = 0; i < idx; i++)
    {
        header = header->list[1];
    }
    return header;
}

value_set **skiplist_make_valueset(skiplist *input, level *from, KEYT *start, KEYT *end)
{
    value_set **res = (value_set **)malloc(sizeof(value_set *) * (input->size + 1));
    memset(res, 0, sizeof(value_set *) * (input->size + 1));
    l_bucket b;
    memset(&b, 0, sizeof(b));
    uint32_t idx = 1;
    snode *target;
    int total_size = 0;
    // 遍历skiplist，起始结尾key赋值给lnode的start key和end key
    for_each_sk(target, input)
    {
        if (idx == 1)
        {
            kvssd_cpy_key(start, &target->key);
        }
        else if (idx == input->size)
        {
            kvssd_cpy_key(end, &target->key);
        }
        idx++;
        if (target->value == 0)
            continue;
        // 这个bucket是一个snode指针数组的集合，每个snode数组存放着相同长度的所有snode指针
        // idx[length]代表了这个长度的snode有多少个
        if (target->value->length > PIECE_PER_PAGE)
        {
            abort();
        }
        if (b.bucket[target->value->length] == NULL)
        {
            b.bucket[target->value->length] = (snode **)malloc(sizeof(snode *) * (input->size + 1));
        }
        b.bucket[target->value->length][b.idx[target->value->length]++] = target;
        total_size += target->value->length;
    }
    int res_idx = 0;
    // 对将刚好需要一个page大小的snode进行操作
    for (int i = 0; i < b.idx[PAGESIZE / PIECE]; i++)
    {
        target = b.bucket[PAGESIZE / PIECE][i];
        res[res_idx] = target->value;
        // 获取当前page ppa
        res[res_idx]->ppa = lsm_get_page(LSM.bm, DATA); // real physical index
        if (bm_env.piece_oob[res[res_idx]->ppa].isvalid == -1)
        {
            abort();
        }
        else
        {
            bm_env.piece_oob[res[res_idx]->ppa].isvalid == 1;
            bm_env.piece_oob[res[res_idx]->ppa].length == PIECE_PER_PAGE;
        }
        target->ppa = res[res_idx]->ppa; // for snode ppa
        target->value = NULL;
        res_idx++;
    }
    b.idx[PAGESIZE / PIECE] = 0;

    for (int i = 1; i < PAGESIZE / PIECE + 1; i++)
    {
        // 如果没有比page小的value返回
        if (b.idx[i] != 0)
            break;
        if (i == PAGESIZE / PIECE)
        {
            return res;
        }
    }
    variable_value2Page(from, &b, &res, &res_idx, false);
    for (int i = 0; i <= PIECE_PER_PAGE; i++)
    { // 将不满page的snode数组指针释放
        if (b.bucket[i])
            free(b.bucket[i]);
    }
    res[res_idx] = NULL;
    return res;
}

skiplist *skiplist_cutting_header(skiplist *in, uint32_t *value)
{
    static uint32_t num_limit = KEYBITMAP / sizeof(uint16_t) - 2; // mitmap need extra 2 for size and length
    static uint32_t size_limit = PAGESIZE - KEYBITMAP;
    // 总数据长度和大小没有到阈值，返回
    if (in->all_length < size_limit && in->size < num_limit)
        return in;

    uint32_t length = 0;
    uint32_t idx = 0;
    snode *temp;
    // 取出limit范围内的节点
    for_each_sk(temp, in)
    {
        length += KEYLEN(temp->key);
        idx++;
        if (length + KEYLEN(temp->list[1]->key) >= size_limit || idx >= num_limit)
            break;
    }
    skiplist *res = skiplist_divide(in, temp);
    res->size = idx;
    res->all_length = length;
    in->size -= idx;
    in->all_length -= length;
    *value = idx;
    return res;
}

skiplist *skiplist_cutting_header_se(skiplist *in, uint32_t *value, KEYT *start, KEYT *end)
{
    static uint32_t num_limit = KEYBITMAP / sizeof(uint16_t) - 2;
    static uint32_t size_limit = PAGESIZE - KEYBITMAP;
    snode *temp;
    uint32_t length = 0;
    uint32_t idx = 0;
    KEYT t_end;
    // 总数据长度和大小没有到阈值，返回(返回前赋值start key和end key)
    if (in->all_length < size_limit && in->size < num_limit)
    {
        for_each_sk(temp, in)
        {
            if (idx == 0)
            {
                kvssd_cpy_key(start, &temp->key);
            }
            t_end = temp->key;
            idx++;
        }
        if (idx != 0)
        {
            kvssd_cpy_key(end, &temp->key);
        }
        return in;
    }

    for_each_sk(temp, in)
    {
        if (idx == 0)
        {
            kvssd_cpy_key(start, &temp->key);
        }
        length += KEYLEN(temp->key);
        idx++;
        t_end = temp->key;
        if (length + KEYLEN(temp->list[1]->key) >= size_limit || idx >= num_limit)
            break;
    }
    kvssd_cpy_key(end, &t_end);
    skiplist *res = skiplist_divide(in, temp);
    res->size = idx;
    res->all_length = length;
    in->size -= idx;
    in->all_length -= length;
    *value = idx;
    return res;
}

uint32_t skiplist_memory_size(skiplist *skip)
{
    if (!skip)
        return 0;
    uint32_t res = 0;
    snode *temp;
    for_each_sk(temp, skip)
    {
        res += sizeof(snode) + temp->level * sizeof(snode *);
        res += temp->key.len;
        res += sizeof(temp->key);
    }
    return res;
}

void *variable_value2Page(level *in, l_bucket *src, value_set ***target_valueset, int *target_valueset_from, bool isgc)
{
    int v_idx;
    /*for normal data*/
    value_set **v_des = NULL;

    /*for gc*/
    htable_t *table_data;
    uint32_t target_ppa;

    v_idx = *target_valueset_from;
    if (isgc)
    { /*v_idx for gc_container*/
        //	gc_container=*((gc_node***)target_valueset);
    }
    else
    { /*v_idx for value_set*/
        v_des = *target_valueset;
    }
    uint32_t gc_write_cnt = 0;
    uint8_t max_piece = PIECE_PER_PAGE - 1; // the max_piece is wrote before enter this section
    // 计算当前最大长(剪枝)
    while (src->idx[max_piece] == 0 && max_piece > 0)
        --max_piece;

    //	bool debuging=false;
    // 从当前最大长度开始遍历
    while (max_piece)
    {
        ppa_t start_ppa = 0;
        PTR page = NULL;
        int ptr = 0;
        int remain = PAGESIZE;

        if (isgc)
        {
            // table_data = (htable_t *)malloc(sizeof(htable_t));
            // page = (PTR)table_data->sets;
            // target_ppa = LSM.lop->moveTo_fr_page(true);
        }
        else
        {
            // 初始化一个value_set
            v_des[v_idx] = inf_get_valueset(page, PAGESIZE);
            // 获取真实ppa
            ppa_t start_ppa = lsm_get_page(LSM.bm, DATA);
            v_des[v_idx]->ppa = start_ppa;
            if (bm_env.piece_oob[v_des[v_idx]->ppa].isvalid == -1)
            {
                abort();
            }
            else
            {
                bm_env.piece_oob[v_des[v_idx]->ppa].isvalid = 1;
                bm_env.piece_oob[v_des[v_idx]->ppa].length = max_piece;
            }
            page = v_des[v_idx]->value;
            // snode ppa赋值
            target_ppa = v_des[v_idx]->ppa;
            // oob
        }
        uint8_t used_piece = 0;
        // varible size allocate in page
        while (remain > 0)
        {
            // 目标长度为剩余长度和max piece的最小值
            int target_length = (remain / PIECE > max_piece ? max_piece : remain / PIECE);
            // 如果当前页长度不够往下遍历，找小于pagesize的
            while (target_length != 0 && src->idx[target_length] == 0)
                --target_length;
            if (target_length == 0)
            {
                break;
            }
            if (isgc)
            {
                // gc_node *target = src->gc_bucket[target_length][src->idx[target_length] - 1];
                // if (!target->plength)
                // {
                //     src->idx[target_length]--;
                //     continue;
                // }
                // target->nppa = LSM.lop->get_page(target->plength, target->lpa);
                // memcpy(&page[ptr], target->value, target_length * PIECE);
            }
            else
            {

                snode *target = src->bucket[target_length][src->idx[target_length] - 1];
                target->ppa = start_ppa;
                if (bm_env.piece_oob[target->ppa].isvalid == -1)
                {
                    abort();
                }
                else
                {
                    bm_env.piece_oob[target->ppa].isvalid = 1;
                    bm_env.piece_oob[target->ppa].length = target_length;
                }
                //memcpy(&page[ptr], target->value->value, target_length * PIECE);
            }
            used_piece += target_length;
            src->idx[target_length]--;
            ptr += target_length * PIECE;
            remain -= target_length * PIECE;
        }

        // if (isgc)
        // {
        //     gc_write_cnt++;
        //     gc_data_write(target_ppa, table_data, GCDW);
        //     free(table_data);
        // }
        // else
        // {
        //     v_idx++;
        // }
        bool stop = 0;
        for (int i = 0; i < PAGESIZE / PIECE + 1; i++)
        {
            if (src->idx[i] != 0)
                break;
            if (i == PAGESIZE / PIECE)
                stop = true;
        }
        if (stop)
            break;
    }

    *target_valueset_from = v_idx;
    if (isgc)
    {
        printf("gc_write_cnt:%d\n", gc_write_cnt);
    }
    return v_des;
}
#endif