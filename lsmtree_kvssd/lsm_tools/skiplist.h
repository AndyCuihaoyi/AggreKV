#ifndef __SKIPLIST_HEADER
#define __SKIPLIST_HEADER
#include <stdint.h>
#include "../lsmtree/lsm_settings.h"
#include "../lsm_tools/container.h"
#include "../lsmtree/lsm_level.h"
#include "value_set.h"

typedef struct value_set value_set;
typedef struct level level;
typedef struct htable table;

#define MAX_L 30 // max level number
#define PROB 4   // the probaility of level increasing : 1/PROB => 1/4

typedef struct footer
{
    uint8_t map[PAGESIZE / PIECE];
} footer;

// travers each snode
#define for_each_sk(node, skip)        \
    for (node = skip->header->list[1]; \
         node != skip->header;         \
         node = node->list[1])

// find identical snode
#define for_each_sk_from(node, from, skip) \
    for (node = from;                      \
         node != skip->header;             \
         node = node->list[1])

// check header
#define SKIPISHEADER(a, b) (a)->header == b ? 1 : 0

// skiplist's node
typedef struct snode
{
    ppa_t ppa;
    KEYT key;
    uint32_t level;
    value_set *value;
    bool isvalid;
    bool iscaching_entry;
    struct snode **list;
    struct snode *back;

    uint64_t etime; // for mapping_update
} snode;

typedef struct length_bucket
{
    snode **bucket[PIECE_PER_PAGE + 1];
#ifdef Lsmtree
    gc_node **gc_bucket[PIECE_PER_PAGE + 1];
#endif
    uint32_t idx[PIECE_PER_PAGE + 1];
    value_set **contents;
    int contents_num;
} l_bucket;

typedef struct skiplist
{
    uint8_t level;       // num of level,default=1(single-level list)
    uint64_t size;       // num of snode
    uint32_t all_length; // length of all key
    uint32_t data_size;  // size of all data
    snode *header;
} skiplist;

// read only iterator. don't using iterater after delete iter's now node
typedef struct
{
    skiplist *list;
    snode *now;
} sk_iter;

skiplist *skiplist_init();                         // return initialized skiplist*
snode *skiplist_find(skiplist *, KEYT);            // find snode having key in skiplist, return NULL:no snode
snode *skiplist_find_lowerbound(skiplist *, KEYT); // find lower bound snode in skiplist
snode *skiplist_range_search(skiplist *, KEYT);
snode *skiplist_strict_range_search(skiplist *, KEYT);
snode *skiplist_insert(skiplist *, request *, bool);                                      // insert skiplist, return inserted snode
snode *skiplist_insert_wP(skiplist *, KEYT, ppa_t, bool);                                 // for gc metadata update with ppa;
value_set **skiplist_make_valueset(skiplist *input, level *from, KEYT *start, KEYT *end); // make value_set from snodes in skiplist
skiplist *skiplist_cutting_header(skiplist *, uint32_t *avg);                             // cut skiplist for compaction
skiplist *skiplist_divide(skiplist *in, snode *target);                                   // assitant to skiplist_cutting_header
skiplist *skiplist_cutting_header_se(skiplist *, uint32_t *avg, KEYT *start, KEYT *end);
snode *skiplist_at(skiplist *, int idx); // find NO.idx snode
int skiplist_delete(skiplist *, KEYT);   // delete by key, return 0:normal -1:empty -2:no key
void skiplist_free(skiplist *list);      // free skiplist
void skiplist_clear(skiplist *list);     // clear all snode in skiplist and  reinit skiplist
void skiplist_print(skiplist *skip);
uint32_t skiplist_memory_size(skiplist *);
void *variable_value2Page(level *in, l_bucket *src, value_set ***target_valueset, int *target_valueset_from, bool isgc);
#endif