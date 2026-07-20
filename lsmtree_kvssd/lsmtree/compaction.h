#ifndef __H_COMPT__
#define __H_COMPT__
#include "lsm_settings.h"
#include "../lsm_tools/skiplist.h"
#include "../lsm_tools/queue.h"
#include "lsmtree.h"
#include <pthread.h>
#define CTHREAD 1
#define CQSIZE 128
#define FLUSH_CHECK_NUM 64
typedef struct compaction_processor compP;
typedef struct compaction_master compM;
typedef struct compaction_req compR;

struct compaction_req
{
    int fromL;
    skiplist *temptable;
    bool last;
    request *parent;
};

typedef struct leveling_node
{
    skiplist *mem;
    KEYT start;
    KEYT end;
    run_t *entry;
} leveling_node;

struct compaction_processor
{
    pthread_t t_id;
    compM *master;
    pthread_mutex_t flag;
    queue *q;
};

struct compaction_master
{
    compP *processors;
    u_int64_t (*pt_leveling)(struct level *, struct level *, struct leveling_node *, struct level *upper);
    bool stopflag;
};

// compaction assistant
bool compaction_init();
void *compaction_main(void *input);
bool compaction_check(request *const req, bool *force);
void compaction_assign(compR *req);
uint32_t compaction_empty_level(level **from, leveling_node *lnode, level **des);
void compaction_sub_pre();
bool htable_read_preproc(run_t *r);
void htable_read_postproc(run_t *r);
void compaction_sub_wait(); // lock
void compaction_sub_post(); // unlock
u_int64_t compaction_subprocessing(struct skiplist *top, struct run **src, struct run **org, struct level *des);
u_int64_t compaction_cascading();
// compaction excution
u_int64_t leveling(level *from, level *to, leveling_node *l_node, pthread_mutex_t *lock);
u_int64_t partial_leveling(level *t, level *origin, leveling_node *lnode, level *upper);
u_int64_t compaction_data_write(leveling_node *lnode);
u_int64_t compaction_htable_write_insert(level *target, run_t *entry);
u_int64_t compaction_htable_read(run_t *ent, PTR *value);
u_int64_t compaction_htable_write(ppa_t ppa, htable *input, KEYT lpa);
level *lsm_level_resizing(level *target, level *src);
uint32_t level_change(level *from, level *to, level *target, pthread_mutex_t *lock);
#endif