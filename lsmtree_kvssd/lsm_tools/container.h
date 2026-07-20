#ifndef __H_CONTAINER__
#define __H_CONTAINER__

#include "../lsmtree/lsm_settings.h"
#include "../x86_64-linux-gnu/bits/pthreadtypes.h"
// algo_req types
#define TRIM 0
#define MAPPINGR 1
#define MAPPINGW 2
#define GCMR 3
#define GCMW 4
#define DATAR 5
#define DATAW 6
#define GCDR 7
#define GCDW 8
#define GCMR_DGC 9
#define GCMW_DGC 10
// end algo_req types
typedef struct lower_info lower_info;
typedef struct keyset
{
    ppa_t ppa;
    KEYT lpa;
} keyset;

typedef struct value_set
{
    uint32_t length;
    uint32_t ppa;
    uint8_t status;
    PTR value;
} value_set;

typedef enum end_type
{
    SKIP_INSERT,
    SKIP_MODIFY,
    SKIP_FLUSH,
    SKIP_FOUND,
    CASCADE_FLUSH,
    NO_COMPACTION,
    NOT_FOUND
} end_type;

typedef struct request
{
    uint8_t type;
    KEYT key;
    value_set *value;
    uint64_t ppa; /*it can be the iter_idx*/
    // for emulate
    uint64_t stime;
    uint64_t etime;
    end_type end_type;
    bool is_compacting;
    pthread_spinlock_t timer_lock;
    void *params;
    void (*end_req)(struct request *const);
    // pthread_mutex_t async_mutex;
    // fdriver_lock_t sync_lock;
} request;

struct algo_req
{
    uint32_t ppa;
    request *parents;
    uint8_t type;
    bool rapid;
    void *(*end_req)(struct algo_req *const);
};

typedef struct algorithm
{
    /*interface*/
    uint32_t (*argument_set)(int argc, char **argv);
    uint32_t (*create)(struct algorithm *, lower_info *);
    void (*destroy)(struct algorithm *, lower_info *);
    uint32_t (*read)(struct algorithm *, request *const);
    uint32_t (*write)(struct algorithm *, request *const);
    uint32_t (*remove)(struct algorithm *, request *const);
    uint32_t (*iter_create)(request *const);
    uint32_t (*iter_next)(request *const);
    uint32_t (*iter_next_with_value)(request *const);
    uint32_t (*iter_release)(request *const);
    uint32_t (*iter_all_key)(request *const);
    uint32_t (*iter_all_value)(request *const);
    uint32_t (*multi_set)(request *const, int num);
    uint32_t (*multi_get)(request *const, int num);
    uint32_t (*range_query)(request *const);

    struct rte_ring *req_q; // for write req in priority
    struct rte_ring *finish_q;
    lower_info *li;

    void *env;
} algorithm;
#endif