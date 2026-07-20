#ifndef __LSM_HEADER__
#define __LSM_HEADER__
#include "lsm_settings.h"
#include "lsm_level.h"
#include "lsm_bm.h"
#include "lsm_pm.h"
#include <pthread.h>
#include <stdbool.h>
#define MAX_INF_REQS (65536)
KEYT key_max, key_min;

enum READTYPE
{
    NOTFOUND,
    FOUND,
    CACHING,
    FLYING
};
typedef struct lsm_request_record
{
    unsigned int data_write_cnt;
    unsigned int back_end_write;
    unsigned int data_read_cnt;
    unsigned int gc_cnt;
    unsigned int check_cnt;
    unsigned int compaction_cnt;
    unsigned int last_compaction_cnt;
} LRR;

typedef struct lsm_set_params
{
    uint8_t LEVELN;
    uint8_t LEVELCACHING;
    uint16_t VALUESIZE;
    uint32_t ONESEGMENT;
    uint32_t KEYNUM;
    uint32_t HEADERNUM; // metapage num(showing size)
    float caching_size;

    uint64_t total_memory;
    uint64_t level_list_memory;
    uint64_t bf_memory;
    uint64_t cache_memory;
    uint64_t pin_memory;
    int64_t remain_memory;
} LSP;

typedef struct lsmtree_levelsize_params
{
    float size_factor;
    float last_size_factor;

    // avg_keynum_inheader real;
    uint32_t avg_keynum_inheader; // metadata segment中key的平均数目
    uint32_t cut_header_cnt;      // 有多少个metadata Segemnt
    uint32_t result_padding;

    // for average value in data segment
    double avg_of_length;
    uint32_t length_cnt;
} LLP;

typedef struct lsmtree
{
    LRR lrr; // lsmtree request record
    LSP lsp; // lsmtree set params
    LLP llp; // lsmtree levelsize params

    uint32_t setup_values;
    uint8_t LEVELN;
    uint8_t LEVELCACHING;
    uint64_t all_header_num;
    // level opertaion sets//
    struct queue *re_q;
    pthread_mutex_t *level_lock;
    pthread_mutex_t memlock;
    pthread_mutex_t templock;
    struct skiplist *memtable;
    struct skiplist *temptable;
    level **disk;
    level *c_level;
    level_ops *lop;

    pthread_mutex_t data_lock;
    ppa_t data_ppa; // for one data caching for read
    value_set *data_value;

    struct cache *lsm_cache;
    int result_padding;
    /*for gc*/
    bool gc_started;
    struct skiplist *gc_list;
    bool gc_compaction_flag;
    // blockmanager *bm;
    // struct lsm_block *active_block;
    bool delayed_header_trim;
    uint32_t delayed_trim_ppa;

    bool debug_flag;
    algorithm *algo;
    block_mgr_t *bm;
    page_t *pm;
} lsmtree;

typedef struct lsm_params
{
    // dl_sync lock;
    uint8_t lsm_type;
    ppa_t ppa;
    void *entry_ptr;
    PTR test;
    PTR *target;
    value_set *value;
    htable *htable_ptr;
    fdriver_lock_t *lock;
} lsm_params;

uint32_t lsm_create(algorithm *, lower_info *);
void lsm_destroy(algorithm *, lower_info *);
uint32_t lsm_set(algorithm *, request *const);
uint32_t lsm_get(algorithm *, request *const);
uint8_t lsm_find_run(KEYT key, run_t **entry, run_t *up_entry, keyset **found, int *level, int *run, int *cxl_access);
int __lsm_get_sub(request *req, run_t *entry, keyset *table, skiplist *list, int idx);
void level_params_print();
htable *htable_assign(char *cpy_src);
void htable_free(htable *input);
htable *htable_copy(htable *input);
#endif