#ifndef __WRITE_BUFFER_H__
#define __WRITE_BUFFER_H__

#include "algo_queue.h"
#include "dftl_types.h"
#include "../tools/skiplist.h"
#include "../tools/d_htable.h"
#include "../tools/rte_ring/rte_ring.h"
#include <pthread.h>
#include <stdint.h>

struct flush_node
{
    ppa_t ppa;
    uint32_t length;
    value_set *value;
    uint32_t value_offt;
};

typedef struct flush_list
{
    int size;
    struct flush_node *list;
} flush_list;

typedef struct wb_env_t
{
    uint32_t max_wb_size;

#ifdef DATA_SEGREGATION
    /* DS 模式：per-stream flush list。每个流 1 个 flush_list，
     * wb path 在 _do_wb_assign_ppa 时按 D_IDX%MAX_GC_STREAM 决定写到
     * 哪个流对应的 fl_stream[stream_idx]。wb 满时所有流统一 flush。 */
    flush_list *fl_stream[MAX_GC_STREAM];
#else
    flush_list *flush_list;
#endif
    struct rte_ring *wb_master_q;
    algo_q *wb_retry_q;
    d_htable *hash_table;

    algorithm *palgo;
} wb_env_t;

typedef struct wb_stats_t
{
    uint64_t nr_rd_hit;
    uint64_t nr_rd_miss;
    uint64_t nr_wr_hit;
    uint64_t nr_wr_miss;
    uint64_t nr_flush;
} wb_stats_t;

typedef struct w_buffer_t
{
    skiplist *wb;
    wb_env_t *env;

    void (*create)(struct w_buffer_t *);
    void (*destroy)(struct w_buffer_t *);
    bool (*is_full)(struct w_buffer_t *);
    uint32_t (*do_check)(struct w_buffer_t *, request *const);
    uint32_t (*insert)(struct w_buffer_t *, request *const);
    void (*assign_ppa)(struct w_buffer_t *, request *const);
    void (*mapping_update)(struct w_buffer_t *, request *const);
    void (*flush)(struct w_buffer_t *, request *const);

    wb_stats_t *stats;
} w_buffer_t;

#endif // __WRITE_BUFFER_H__