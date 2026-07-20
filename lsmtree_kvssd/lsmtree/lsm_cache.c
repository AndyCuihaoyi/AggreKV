#include "lsmtree.h"
#include "lsm_cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int32_t update, delete_, insert;
cache *cache_init(uint32_t noe)
{
    cache *c = (cache *)malloc(sizeof(cache));
    c->m_size = noe;
    c->n_size = 0;
    c->top = NULL;
    c->bottom = NULL;
    c->max_size = noe;
    pthread_mutex_init(&c->cache_lock, NULL);
    return c;
}

void cache_evict(cache *c)
{
    cache_delete(c, cache_get(c));
}

void cache_size_update(cache *c, int m_size)
{
    if (m_size < 0)
        return;
    if (c->n_size > m_size)
    {
        int i = 0;
        int target = c->n_size - m_size;
        for (i = 0; i < target; i++)
        {
            cache_evict(c);
        }
    }
    c->m_size = m_size > c->max_size ? c->max_size : m_size;
}

cache_entry *cache_insert(cache *c, run_t *ent, int dmatag)
{
    if (!c->m_size)
        return NULL;
    // 清除cache中的entry，至少留下一个可以插入
    if (c->m_size < c->n_size)
    {
        int target = c->n_size - c->m_size + 1;
        for (int i = 0; i < target; i++)
        {
            if (!cache_delete(c, cache_get(c)))
                return NULL;
        }
    }

    // 插入计数++，缓存指针指向run_t
    insert++;
    cache_entry *c_ent = (cache_entry *)malloc(sizeof(cache_entry));
    c_ent->locked = false;
    c_ent->entry = ent;
    // cache为空
    if (c->bottom == NULL)
    {
        c->bottom = c_ent;
        c->top = c_ent;
        c->bottom->down = NULL;
        c->top->up = NULL;
        c->n_size++;
        return c_ent;
    }
    else // cache中有其他节点
    {
        c->top->up = c_ent;
        c_ent->down = c->top;
        c->top = c_ent;
        c_ent->up = NULL;
        c->n_size++;
    }
    // printf("cache insert:%d\n",c->n_size);
    return c_ent;
}
bool cache_delete(cache *c, run_t *ent)
{
    delete_++;
    if (c->n_size == 0 || !ent)
    {
        return false;
    }
    // printf("cache delete\n");
    cache_entry *c_ent = ent->c_entry;
    if (c_ent == c->bottom)
    { // 在底部的删除
        c->bottom = c_ent->up;
    }
    else if (c_ent == c->top)
    { // 在顶部的删除
        c->top = c_ent->down;
    }
    // if(!ISNOCPY(LSM.setup_values)) htable_free(ent->cache_data);
    // delete函数主要完成下面工作，断链重建的工作用get配合已经完成了
    c->n_size--;
    free(c_ent);
    ent->c_entry = NULL;
    return true;
}

bool cache_delete_entry_only(cache *c, run_t *ent)
{
    if (c->n_size == 0)
    {
        return false;
    }
    cache_entry *c_ent = ent->c_entry;
    if (c_ent == NULL)
    {
        return false;
    }
    if (c->bottom == c->top && c->top == c_ent)
    {
        c->top = c->bottom = NULL;
    }
    else if (c->top == c_ent)
    {
        cache_entry *down = c_ent->down;
        down->up = NULL;
        c->top = down;
    }
    else if (c->bottom == c_ent)
    {
        cache_entry *up = c_ent->up;
        up->down = NULL;
        c->bottom = up;
    }
    else
    {
        cache_entry *up = c_ent->up;
        cache_entry *down = c_ent->down;

        up->down = down;
        down->up = up;
    }
    c->n_size--;
    free(c_ent);
    ent->c_entry = NULL;
    return true;
}

// 把对应的cache_entry放在顶部
void cache_update(cache *c, run_t *ent)
{
    // 更新次数++
    update++;
    cache_entry *c_ent = ent->c_entry;
    if (c->top == c_ent)
    {
        return;
    }
    if (c->bottom == c_ent)
    {
        cache_entry *up = c_ent->up;
        up->down = NULL;
        c->bottom = up;
    }
    else
    {
        cache_entry *up = c_ent->up;
        cache_entry *down = c_ent->down;
        up->down = down;
        down->up = up;
    }

    c->top->up = c_ent;
    c_ent->up = NULL;
    c_ent->down = c->top;
    c->top = c_ent;

    if (c->m_size < c->n_size)
    {
        int target = c->n_size - c->m_size + 1;
        for (int i = 0; i < target; i++)
        {
            cache_delete(c, cache_get(c));
        }
    }
}

run_t *cache_get(cache *c) // 配合delete函数完成操作,get取出节点后重新建立
{
    if (c->n_size == 0)
    {
        return NULL;
    }

    //	cache_entry *res=c->bottom;
    //	cache_entry *up=res->up;
    int c_cnt = 1;
    cache_entry *res = c->bottom;
    cache_entry *up;
    while (res && res->locked)
    {
        res = res->up;
        c_cnt = 0;
    }
    if (res)
        up = res->up;
    else
        return NULL;

    if (up == NULL && c_cnt)
    {
        c->bottom = c->top = NULL;
    }
    else if (c_cnt && res->locked)
    {
        return NULL;
    }
    else
    {
        // 要删除的是最底部的节点
        if (res == c->bottom)
        {
            up->down = NULL;
            c->bottom = up;
        }
        else
        {
            if (up)
            { // 如果是夹在中间的节点段链重接
                up->down = res->down;
                res->down->up = up;
            }
            else // 如果是最顶部的节点
                res->down->up = NULL;
        }
    }

    if (!res->entry->c_entry || res->entry->c_entry != res)
    {
        cache_print(c);
        printf("hello\n");
    }
    return res->entry;
}

// 释放没锁定的缓存
void cache_free(cache *c)
{
    run_t *tmp_ent;
    printf("cache size:%d %d %d\n", c->n_size, c->m_size, c->max_size);
    while ((tmp_ent = cache_get(c)))
    {
        free(tmp_ent->c_entry);
        tmp_ent->c_entry = NULL;
        c->n_size--;
    }
    free(c);
    printf("insert:%u delete:%d update:%u\n", insert, delete_, update);
}

int print_number;
// 逐个检查指针指向的run和run指向的entry能不能对上，不能的话打印fuck
void cache_print(cache *c)
{
    cache_entry *start = c->top;
    print_number = 0;
    run_t *tent;
    while (start != NULL)
    {
        tent = start->entry;
        if (start->entry->c_entry != start)
        {
            printf("fuck!!!\n");
        }
#ifdef KVSSD
        if (ISNOCPY(LSM.setup_values))
        {
            // printf("[%d]c->endtry->key:%s c->entry->pbn:%lu d:%p\n",print_number++,kvssd_tostring(tent->key),tent->pbn,tent->cache_nocpy_data_ptr);
        }
#else
        // printf("[%d]c->entry->key:%d c->entry->pbn:%d d:%p\n",print_number++,tent->key,tent->pbn,tent->cache_data);
#endif
        start = start->down;
    }
}

bool cache_insertable(cache *c)
{
    // printf("m:n %d:%d\n", c->m_size, c->n_size);
    // return c->m_size == 0 ? 0 : 1;
}

// 锁定
void cache_entry_lock(cache *c, cache_entry *entry)
{
    c->locked_entry++;
    entry->locked = true;
}

// 解锁
void cache_entry_unlock(cache *c, cache_entry *entry)
{
    c->locked_entry--;
    entry->locked = false;
}
