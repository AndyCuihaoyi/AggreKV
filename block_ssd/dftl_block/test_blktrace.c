
#include "test_blktrace.h"
#include "../lower/lower.h"
#include "../tools/blktrace/blktrace.h"
#include "../tools/random/zipf.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/valueset.h"
#include "algo_queue.h"
#include "cache.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "dftl_wb.h"
#include "request.h"
#include <getopt.h>
#include <glib-2.0/glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#define NUM_ITEMS (44000000)
#define NUM_UPDATES (44000000)
#define NUM_WORKERS (6)
#define READ_ALPHA (0.99)
#define UPDATE_ALPHA (0.99)
volatile bool thread_ended[NUM_WORKERS] = {false};
bool full_worker = true;
// #define VALUE_CHECK

bool update_running = false;
volatile int gc_inflight = 0;

#define MAX_TOTAL_SAMPLES (67200000)
static uint64_t *total_latency_samples = NULL;
static uint64_t total_sample_count = 0;
#define MAX_YCSB_SAMPLES (67200000)
static uint64_t ycsb_sample_count = 0;
static uint64_t *ycsb_latency_samples = NULL;
#define MAX_GC_SAMPLES (9000000)
static uint64_t *gc_latency_samples = NULL;
static uint64_t gc_sample_count = 0;
uint32_t hit_cnt_array[1000000];

uint64_t ycsb_total_batch_avg_lat_us = 0;
uint64_t *shuffle_map = NULL;
extern lower_info ssd_li;
int iodepth = 1;
pthread_spinlock_t global_inner_timer_lock;
struct req_inner_timer global_inner_timer[INNER_TIMER_SIZE] = {0};

// void submit_req(algorithm *palgo, request *req);

static uint64_t finished_r = 0;
static uint64_t finished_w = 0;
static uint64_t finished_total = 0;
static uint64_t wrong_value_cnt = 0;
static uint64_t total_hit_inpage = 0;
static uint64_t total_inpage = 0;
static float avg_hit_inpage = 0.0;
#ifdef VALUE_CHECK
char *values[NUM_ITEMS] = {0};
#endif
#define BATCH_SIZE (200000)
#define MAX_LATENCY_US 100000000 // 10s

static uint64_t total_lat_arr[MAX_LATENCY_US] = {0};
static uint64_t pt_total_lat_arr = 0;

static uint64_t total_start_ns = 0;
static uint64_t total_batch_lat_ns = 0;

static uint64_t complete_total_slow = 0;

static double total_iops = 0;
static double gc_iops = 0;
static uint64_t total_batch_cnt = 0;
static uint64_t gc_batch_cnt = 0;
static uint64_t total_blktrace_requests = 0;

extern uint64_t timer_start_ns;

pthread_t algo_tr;
pthread_t finish_tr[2];

int compare_uint32(const void *a, const void *b);

int compare_uint32(const void *a, const void *b) {
    const uint32_t *val_a = (const uint32_t *)a;
    const uint32_t *val_b = (const uint32_t *)b;
    if (*val_a < *val_b)
        return -1;
    if (*val_a > *val_b)
        return 1;
    return 0;
}

void wait_for_nr_ios(request *req) {
    while (1) {
        if ((*req->ptr_nr_ios) < iodepth) {
            g_atomic_int_add(req->ptr_nr_ios, 1);
            return;
        }
    }
}

void submit_req(algorithm *palgo, request *req) {
    wait_for_nr_ios(req);
    pthread_spin_init(&req->timer_lock, PTHREAD_PROCESS_PRIVATE);
    req->stime = req->etime = clock_get_ns();
    while (!ring_enqueue(palgo->req_q, (void *)&req, 1))
        ;
    ftl_assert((*req->ptr_nr_ios) <= iodepth);
}

static void record_total_request_stats(request *req, uint64_t latency_ns,
                                       uint64_t latency_us) {
    uint64_t finished_total_local =
        __atomic_add_fetch(&finished_total, 1, __ATOMIC_RELAXED);
    uint64_t sum_total_lat_ns_local =
        __atomic_add_fetch(&total_batch_lat_ns, latency_ns, __ATOMIC_RELAXED);
    uint64_t sample_idx =
        __atomic_fetch_add(&total_sample_count, 1, __ATOMIC_RELAXED);

    if (sample_idx < MAX_TOTAL_SAMPLES) {
        total_latency_samples[sample_idx] = latency_us;
    }

    if (latency_us < MAX_LATENCY_US) {
        g_atomic_pointer_add(&total_lat_arr[latency_us], 1);
        if (latency_us > pt_total_lat_arr) {
            g_atomic_pointer_set(&pt_total_lat_arr, latency_us);
        }
    }

    if (finished_total_local % BATCH_SIZE == 0) {
        uint64_t now = clock_get_ns();
        double elapsed = (now - total_start_ns) / 1e9;
        double batch_iops = elapsed > 0 ? (double)BATCH_SIZE / elapsed : 0;
        double batch_kvops = total_blktrace_requests && test_env.num_upper_level_kvops
                                 ? batch_iops *
                                       (double)test_env.num_upper_level_kvops /
                                       (double)total_blktrace_requests
                                 : 0;

        total_iops += batch_iops;
        total_batch_cnt++;
        total_start_ns = now;
        ftl_log("%lu th request end. key: %s, iops: %lf, kvops: %lf, avg_lat: "
                "%ld, slow: %ld\n",
                finished_total_local, req->key.key, batch_iops, batch_kvops,
                sum_total_lat_ns_local / BATCH_SIZE, complete_total_slow);
        g_atomic_pointer_set(&total_batch_lat_ns, 0);
        complete_total_slow = 0;
        fflush(stdout);
        fflush(stderr);
    }
}

void *end_request(request *req) {
    // req->etime = clock_get_ns();
    switch (req->type) {
    case DATAR:
        if (req->state == ALGO_REQ_NOT_FOUND) {
            ftl_log("%lu th read request end. key: %s, value not found\n",
                    ++finished_r, req->key.key);
        } else {
            uint64_t latency = req->etime - req->stime;
            uint64_t latency_us = latency / 1000;
            uint64_t finished_r_local =
                __atomic_add_fetch(&finished_r, 1, __ATOMIC_RELAXED);
            if (finished_r_local == 16000000) {
                struct cmt_struct *target_cmt;
                NODE *current_node = lru_get_target_node(d_cache.member.lru, NULL);

                if (current_node == NULL) {
                    ftl_warn("LRU list is empty, no node to iterate\n");
                }
                while (current_node != NULL) {
                    target_cmt = (struct cmt_struct *)current_node->DATA;

                    if (target_cmt != NULL) {
                        total_inpage++;
                        total_hit_inpage += target_cmt->hit_cnt;
                    }
                    current_node = lru_get_target_node(NULL, current_node);
                }
                avg_hit_inpage = (float)total_hit_inpage / total_inpage;
            }
            record_total_request_stats(req, latency, latency_us);
        }
        if (req->value) {
            inf_free_valueset(&req->value);
        }
        break;
    case DATAW: {
        uint64_t latency = req->etime - req->stime;
        uint64_t latency_us = latency / 1000;

        __atomic_add_fetch(&finished_w, 1, __ATOMIC_RELAXED);
        record_total_request_stats(req, latency, latency_us);
        if (gc_sample_count < MAX_GC_SAMPLES &&
            __atomic_load_n(&gc_inflight, __ATOMIC_RELAXED) > 0) {
            gc_latency_samples[gc_sample_count++] = latency_us;
        }
    } break;
    }
    for (int i = 0; i < INNER_TIMER_SIZE; ++i) {
        if (req->inner_timer[i].elapsed) {
            pthread_spin_lock(&global_inner_timer_lock);
            global_inner_timer[i].elapsed += req->inner_timer[i].elapsed;
            pthread_spin_unlock(&global_inner_timer_lock);
        }
    }
    if (req->h_params) {
        free(req->h_params);
        req->h_params = NULL;
    }
    if (req->params) {
        free(req->params);
        req->params = NULL;
    }
    pthread_spin_destroy(&req->timer_lock);
    g_atomic_int_add(req->ptr_nr_ios, -1);
    free(req);
    return NULL;
}

void *algo_thread() {
    prctl(PR_SET_NAME, "algo_thread");
    algorithm *palgo = &__demand;
    while (1) {
        request *req = NULL;
        uint64_t now = clock_get_ns();
        while (palgo->retry_q->head != NULL &&
               ((request *)palgo->retry_q->head->payload)->etime <= now) {
            req = algo_q_dequeue(palgo->retry_q);
            switch (req->type) {
            case DATAR:
                palgo->read(palgo, req);
                break;
            case DATAW:
                palgo->write(palgo, req);
                break;
            default:
                break;
            }
            now = clock_get_ns();
        }
        if (ring_count(palgo->req_q) > 0) {
            ring_dequeue(palgo->req_q, (void *)&req, 1);
            req->etime = clock_get_ns();
            switch (req->type) {
            case DATAR:
                palgo->read(palgo, req);
                break;
            case DATAW:
                palgo->write(palgo, req);
                break;
            default:
                break;
            }
        }
    }
}

void check_dcache_queue() {
    demand_cache *pd_cache = &d_cache;
    struct cmt_struct **cmt = pd_cache->member.cmt;
    for (int i = 0; i < d_cache.env.nr_valid_tpages; ++i) {
        int retry_cnt = cmt[i]->retry_q ? cmt[i]->retry_q->size : 0;
        int wait_cnt = cmt[i]->wait_q ? cmt[i]->wait_q->size : 0;
        if (retry_cnt || wait_cnt) {
            ftl_err("cmt[%d]: retry: %d, wait: %d\n", i, retry_cnt, wait_cnt);
        }
    }
    ftl_log("cmt check finished.\n");
    return;
}

void *process_cq_cpl() {
    prctl(PR_SET_NAME, "cq_cpl_thread");
    algorithm *palgo = &__demand;
    request *req = NULL;
    algo_q *complete_q = algo_q_create();
    while (1) {
        uint64_t now = clock_get_ns();
        if (ring_dequeue(palgo->finish_q, (void *)&req, 1)) {
            if (req->etime <= now) {
                req->end_req(req);
            } else {
                algo_q_insert_sorted(complete_q, req, NULL);
            }
        }
        if (complete_q->head) {
            uint64_t now = clock_get_ns();
        retry:
            req = (request *)complete_q->head->payload;
            if (req->etime <= now) {
                if (now - req->etime > 10000) {
                    complete_total_slow++;
                }
                algo_q_dequeue(complete_q);
                req->end_req(req);
                if (complete_q->head) {
                    goto retry;
                }
            }
        }
    }
    return NULL;
}

void test_insert_read(algorithm *palgo) {
    for (uint64_t i = 1; i < NUM_ITEMS; ++i) {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
#ifdef VALUE_CHECK
        if (!values[i]) {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    for (uint64_t i = 1; i < NUM_ITEMS; ++i) {
        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        memcpy(r_req->key.key, key, keylen);
        r_req->key.len = keylen;
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        submit_req(palgo, r_req);
    }
}

void test_insert_update_read(algorithm *palgo) {
    // insert
    for (uint64_t i = 1; i < NUM_ITEMS; ++i) {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);

#ifdef VALUE_CHECK
        if (!values[i]) {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    // update
    for (uint64_t i = 1; i < NUM_UPDATES; ++i) {
        int rndkey = rand() % (NUM_ITEMS - 1) + 1;
        int rnd = rand() % (NUM_ITEMS - 1) + 1;
        // int len = rand() % 3500 + 9;    // variable, must larger than
        // len(str(rnd))
        int len = 512; // fixed, must larger than len(str(rnd))
        char value_str[len];
        memset(value_str, 0, len);
        sprintf(value_str, "%0*d", len - 1, rnd);
        value_set *value = inf_get_valueset(value_str, len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%d", rndkey);

#ifdef VALUE_CHECK
        if (values[atoi(key)]) {
            sprintf(values[atoi(key)], "%s", value->value);
        } else {
            ftl_err("value[%lu] is NULL\n", i);
        }
#endif

        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    // read
    for (uint64_t i = 1; i < NUM_ITEMS; ++i) {
        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        memcpy(r_req->key.key, key, keylen);
        r_req->key.len = keylen;
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        submit_req(palgo, r_req);
    }
}

void test_load(algorithm *palgo, uint64_t num) {
    // load
    ftl_log("load workload size: %.2f GB\n", (double)num * PIECE / G);
    total_start_ns = clock_get_ns();
    volatile int tr_nr_ios = 0;
    for (uint64_t i = 1; i < num + 1; ++i) {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
#ifdef VALUE_CHECK
        if (!values[i]) {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->key.len = sprintf(w_req->key.key, "%lu", i);
        w_req->type = DATAW;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }
    usleep(2000000);
}

void *test_load_wrapper_tr(void *args) {
    test_worker_args *pargs = args;
    algorithm *palgo = pargs->palgo;
    uint64_t num = pargs->num;

    while (!(*pargs->pstart))
        ;

    test_load(palgo, num);

    return NULL;
}

void *test_update_tr(void *args) {
    // update
    test_worker_args *pargs = args;
    algorithm *palgo = pargs->palgo;
    uint64_t max = pargs->max;
    uint64_t num = pargs->num;
    bool is_zipf = pargs->is_zipf;
    int seed = pargs->seed;
    volatile int tr_nr_ios = 0;

    struct zipf_state zs;
    srand(seed);
    zipf_init(&zs, max - 1, UPDATE_ALPHA, -1, seed);
    zipf_disable_hash(&zs);
    while (!(*pargs->pstart))
        ;
    for (uint64_t i = 0; i < num; ++i) {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        uint64_t rndkey;
        // int rnd = rand() % (max - 1) + 1;
        if (is_zipf)
            rndkey = zipf_next(&zs) + 1;
        else
            rndkey = rand() % (max - 1) + 1;
        // int len = rand() % 3500 + 9;    // variable, must larger than
        // len(str(rnd)) int len = 512;    // fixed, must larger than len(str(rnd))
        char value_str[len];
        memset(value_str, 0, len);
        // sprintf(value_str, "%0*d", len - 1, rnd);
        // value_set *value = inf_get_valueset(value_str, len);
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        w_req->key.len = sprintf(w_req->key.key, "%lu", rndkey);
        // #ifdef VALUE_CHECK
        //         if (values[atoi(key)]) {
        //             sprintf(values[atoi(key)], "%s", value->value);
        //         } else {
        //             ftl_err("value[%lu] is NULL\n", i);
        //         }
        // #endif
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }
    usleep(2000000);
    return NULL;
}

void test_update(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf,
                 int seed, int nr_workers) {
    // update
    D_ENV(palgo)->num_rd_data_rd = 0;
    update_running = true;
    total_start_ns = clock_get_ns();
    pthread_t workers[nr_workers];
    volatile bool start = false;
    test_worker_args args = {.palgo = palgo,
                             .max = max,
                             .num = num,
                             .is_zipf = is_zipf,
                             .seed = seed,
                             .blktrace = NULL,
                             .pstart = &start};
    for (int i = 0; i < nr_workers; ++i) {
        pthread_create(&workers[i], NULL, test_update_tr, &args);
    }
    sleep(5);
    start = true;
    for (int i = 0; i < nr_workers; ++i) {
        pthread_join(workers[i], NULL);
    }
}

void *test_read_tr(void *args) {
    // read
    static int worker_cnt = 0;
    int worker_id = g_atomic_int_add(&worker_cnt, 1);
    prctl(PR_SET_NAME, "read_worker");

    test_worker_args *pargs = args;

    algorithm *palgo = pargs->palgo;
    uint64_t max = pargs->max;
    uint64_t num = pargs->num;
    bool is_zipf = pargs->is_zipf;
    bool is_seq = pargs->is_seq;
    uint32_t seed = pargs->seed + worker_id;
    volatile int tr_nr_ios = 0;
    struct zipf_state zs;

    if (is_zipf && !is_seq) {
        zipf_init(&zs, max - 1, READ_ALPHA, -1, seed);
        if (shuffle_map)
            ;
        zipf_use_shuffle_map(&zs, shuffle_map);
    }

    srand(seed);

    while (!(*pargs->pstart))
        ;
    for (uint64_t i = 0; i < num; ++i) {
        uint64_t rndkey;
        if (is_seq) {
            rndkey = (uint64_t)worker_id * num + i + 1;
        } else if (is_zipf) {
            rndkey = zipf_next(&zs) + 1;
        } else {
            rndkey = rand_r(&seed) % (max - 1) + 1;
        }

        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        r_req->key.len = sprintf(r_req->key.key, "%lu", rndkey);
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        r_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, r_req);
    }

    thread_ended[worker_id] = true;
    usleep(2000000);
    return NULL;
}

void test_read(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf,
               bool is_seq, int seed, int nr_workers) {
    // read
    ftl_log("read workload size: %.2f GB\n",
            (double)num * NUM_WORKERS * PIECE / G);
    total_start_ns = clock_get_ns();
    pthread_t workers[nr_workers];
    volatile bool start = false;
    test_worker_args args = {.palgo = palgo,
                             .max = max,
                             .num = num,
                             .is_zipf = is_zipf,
                             .is_seq = is_seq,
                             .seed = seed,
                             .blktrace = NULL,
                             .pstart = &start};
    for (int i = 0; i < nr_workers; ++i) {
        pthread_create(&workers[i], NULL, test_read_tr, &args);
    }
    sleep(5);
    start = true;
    for (int i = 0; i < nr_workers; ++i) {
        pthread_join(workers[i], NULL);
    }
}

void show_latency_stats() {
    uint64_t avg_lat = 0;
    uint64_t nr_req = 0;
    for (uint64_t i = 0; i <= pt_total_lat_arr; i++) {
        if (total_lat_arr[i]) {
            nr_req += total_lat_arr[i];
            avg_lat += total_lat_arr[i] * i;
        }
    }
    if (nr_req) {
        avg_lat /= nr_req;
    }
    ftl_log("average request latency: %lu us\n", avg_lat);
}

void init_global_timer() {
    pthread_spin_init(&global_inner_timer_lock, PTHREAD_PROCESS_PRIVATE);
}

void clean_stats() {
    __atomic_store_n(&gc_inflight, 0, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < MAX_LATENCY_US; i++) {
        total_lat_arr[i] = 0;
    }
    pt_total_lat_arr = 0;
    ssd_li.stats->nr_nand_read = ssd_li.stats->nr_nand_write =
        ssd_li.stats->nr_nand_erase = 0;
    finished_r = finished_w = finished_total = 0;
    for (int i = 0; i <= d_env.max_try; ++i)
        d_env.r_hash_collision_cnt[i] = 0;
    for (int i = 0; i <= d_env.max_try; ++i)
        d_env.w_hash_collision_cnt[i] = 0;
    write_buffer.stats->nr_rd_hit = write_buffer.stats->nr_rd_miss =
        write_buffer.stats->nr_wr_hit = write_buffer.stats->nr_wr_miss = 0;
    d_cache.stat.cache_hit = d_cache.stat.cache_miss =
        d_cache.stat.cache_miss_by_collision =
            d_cache.stat.cache_hit_by_collision = 0;
    d_env.num_rd_data_rd = d_env.num_rd_data_miss_rd = 0;
    d_cache.stat.dirty_evict = d_cache.stat.cache_load = 0;
    for (int i = 0; i < sizeof(ssd_li.stats->nr_nand_rd_lun); ++i) {
        ssd_li.stats->nr_nand_rd_lun[i] = ssd_li.stats->nr_nand_wr_lun[i] =
            ssd_li.stats->nr_nand_er_lun[i] = 0;
    }
#ifdef HOT_CMT
    d_cache.stat.hot_cmt_hit = 0;
    d_cache.stat.hot_rewrite_entries = 0;
    d_cache.stat.hot_valid_entries = 0;
    d_cache.stat.up_grain_cnt = 0;
    d_cache.stat.up_hit_cnt = 0;
    d_cache.stat.up_page_cnt = 0;
    memset(d_cache.stat.grain_heat_distribute, 0, sizeof(uint32_t) * 1000);
#endif
    total_sample_count = 0;
    total_iops = 0;
    total_batch_cnt = 0;
    total_batch_lat_ns = 0;
    gc_sample_count = 0;
    ycsb_sample_count = 0;
    gc_iops = gc_batch_cnt = 0;
    complete_total_slow = 0;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t val_a = *(const uint64_t *)a;
    uint64_t val_b = *(const uint64_t *)b;
    if (val_a < val_b)
        return -1;
    if (val_a > val_b)
        return 1;
    return 0;
}

static uint64_t calculate_tail_latency(uint64_t *samples, uint64_t sample_count,
                                       double percentile) {
    if (sample_count == 0)
        return 0;

    qsort(samples, sample_count, sizeof(uint64_t), compare_uint64);

    double index = percentile / 100.0 * (sample_count - 1);
    uint64_t floor_idx = (uint64_t)index;
    uint64_t ceil_idx = floor_idx + 1;
    double frac = index - floor_idx;

    if (ceil_idx >= sample_count) {
        return samples[floor_idx];
    }
    return (uint64_t)(samples[floor_idx] * (1 - frac) + samples[ceil_idx] * frac);
}

static void show_tail_latency_stats() {
    uint64_t effective_total_sample_count =
        total_sample_count < MAX_TOTAL_SAMPLES ? total_sample_count : MAX_TOTAL_SAMPLES;
    if (effective_total_sample_count == 0) {
        ftl_log("No request latency samples collected\n");
    } else {
        uint64_t sum_lat_us = 0;
        for (uint64_t i = 0; i < effective_total_sample_count; i++) {
            sum_lat_us += total_latency_samples[i];
        }
        uint64_t avg_lat_us = sum_lat_us / effective_total_sample_count;
        uint64_t p95_lat_us = calculate_tail_latency(
            total_latency_samples, effective_total_sample_count, 95.0);
        uint64_t p99_lat_us = calculate_tail_latency(
            total_latency_samples, effective_total_sample_count, 99.0);
        uint64_t p99_9_lat_us = calculate_tail_latency(
            total_latency_samples, effective_total_sample_count, 99.9);
        uint64_t p99_99_lat_us = calculate_tail_latency(
            total_latency_samples, effective_total_sample_count, 99.99);
        ftl_log("========== Tail Latency Stats (iodepth=%d) ==========\n", iodepth);
        ftl_log("request samples count: %lu\n", effective_total_sample_count);
        ftl_log("avg_lat/us: %lu | p95_lat/us: %lu | p99_lat/us: %lu | "
                "p99.9_lat/us: %lu | p99.99_lat/us: %lu\n",
                avg_lat_us, p95_lat_us, p99_lat_us, p99_9_lat_us,
                p99_99_lat_us);
    }
    if (ycsb_sample_count == 0) {
        ftl_log("No YCSB latency samples collected\n");
    } else {
        uint64_t sum_ycsblat_us = 0;
        for (uint64_t i = 0; i < ycsb_sample_count; i++) {
            sum_ycsblat_us += ycsb_latency_samples[i];
        }
        uint64_t avg_ycsblat_us = sum_ycsblat_us / ycsb_sample_count;
        uint64_t p95_ycsblat_us =
            calculate_tail_latency(ycsb_latency_samples, ycsb_sample_count, 95.0);
        uint64_t p99_ycsblat_us =
            calculate_tail_latency(ycsb_latency_samples, ycsb_sample_count, 99.0);
        uint64_t p99_9_ycsblat_us =
            calculate_tail_latency(ycsb_latency_samples, ycsb_sample_count, 99.9);
        ftl_log("========== YCSB Tail Latency Stats (iodepth=1) ==========\n");
        ftl_log("ycsb samples count: %lu\n", ycsb_sample_count);
        ftl_log("avg_ycsblat/us: %lu | p95_ycsblat/us: %lu | p99_ycsblat/us: %lu | "
                "p99.9_ycsblat/us: %lu\n",
                avg_ycsblat_us, p95_ycsblat_us, p99_ycsblat_us, p99_9_ycsblat_us);
    }
    if (gc_sample_count == 0) {
        ftl_log("No GC latency samples collected\n");
    } else {
        uint64_t sum_gclat_us = 0;
        for (uint64_t i = 0; i < gc_sample_count; i++) {
            sum_gclat_us += gc_latency_samples[i];
        }
        uint64_t avg_gclat_us = sum_gclat_us / gc_sample_count;
        uint64_t p95_gclat_us =
            calculate_tail_latency(gc_latency_samples, gc_sample_count, 95.0);
        uint64_t p99_gclat_us =
            calculate_tail_latency(gc_latency_samples, gc_sample_count, 99.0);
        uint64_t p99_9_gclat_us =
            calculate_tail_latency(gc_latency_samples, gc_sample_count, 99.9);
        ftl_log("========== GC Tail Latency Stats (iodepth=1) ==========\n");
        ftl_log("gc samples count: %lu\n", gc_sample_count);
        ftl_log("avg_gclat/us: %lu | p95_gclat/us: %lu | p99_gclat/us: %lu | "
                "p99.9_gclat/us: %lu\n",
                avg_gclat_us, p95_gclat_us, p99_gclat_us, p99_9_gclat_us);
    }
}

void show_stats() {
    double avg_total_iops = total_batch_cnt ? total_iops / total_batch_cnt : 0;
    double kvops = total_blktrace_requests && test_env.num_upper_level_kvops
                       ? avg_total_iops *
                             (double)test_env.num_upper_level_kvops /
                             (double)total_blktrace_requests
                       : 0;

    show_latency_stats();
    ftl_log("finished_total: %lu, wrong_value_cnt: %lu\n", finished_total,
            wrong_value_cnt);
    ftl_log("data_rd: %lu, mapping_rd: %lu, mapping_wr: %lu\n",
            d_env.num_rd_data_rd, d_cache.stat.cache_load,
            d_cache.stat.dirty_evict);
    ftl_log("nand_r: %lu, nand_w: %lu, nand_e: %lu\n", ssd_li.stats->nr_nand_read,
            ssd_li.stats->nr_nand_write, ssd_li.stats->nr_nand_erase);
    // for (int i = 0; i <= d_env.max_try; ++i)
    //     ftl_log("r_hash_collision[%d]: %lu\n", i,
    //     d_env.r_hash_collision_cnt[i]);
    // for (int i = 0; i <= d_env.max_try; ++i)
    //     ftl_log("w_hash_collision[%d]: %lu\n", i,
    //     d_env.w_hash_collision_cnt[i]);
    // ftl_log("w_buffer: flush: %lu, w_buffer: rd_hit: %lu, rd_miss: %lu, wr_hit:
    // %lu, wr_miss: %lu\n",
    //         write_buffer.stats->nr_flush, write_buffer.stats->nr_rd_hit,
    //         write_buffer.stats->nr_rd_miss, write_buffer.stats->nr_wr_hit,
    //         write_buffer.stats->nr_wr_miss);
#ifdef HOT_CMT
    ftl_log("total iops: %0.2f, hit rt:%0.2f%%\n",
            avg_total_iops,
            (double)(d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit) /
                (d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit +
                 d_cache.stat.cache_miss) *
                100);
    ftl_log("hot valid entries: %lu, hot valid pages: %lu, max hot pages: %lu\n",
            d_cache.stat.hot_valid_entries, d_cache.stat.hot_valid_entries / EPP,
            d_cache.env.max_cached_hot_tpages);
    double avg_grain_per_page =
        (double)d_cache.stat.up_grain_cnt / d_cache.stat.up_page_cnt;
    double avg_grain_hit =
        (double)d_cache.stat.up_hit_cnt / d_cache.stat.up_grain_cnt;
    ftl_log("avg_grain_per_page: %.2f, avg_grain_hit: %.2f\n", avg_grain_per_page,
            avg_grain_hit);
    ftl_log("hot_hit: %lu, hot_rewrite_entries: %lu, up_page_cnt:%lu, "
            "up_grain_cnt:%lu, equal_up_page:%lu \n",
            d_cache.stat.hot_cmt_hit, d_cache.stat.hot_rewrite_entries,
            d_cache.stat.up_page_cnt, d_cache.stat.up_grain_cnt,
            d_cache.stat.up_grain_cnt / EPP);
    for (int i = 0; i < 10; i++) {
        if (d_cache.stat.grain_heat_distribute[i] > 0)
            ftl_log("grain_heat_distribute[%d]: %0.6f%%\n", i,
                    (double)d_cache.stat.grain_heat_distribute[i] /
                        d_cache.stat.up_page_cnt / EPP * 100);
    }
#else
    ftl_log("total iops: %0.2f,hit rt:%0.2f%%\n",
            avg_total_iops,
            (d_cache.stat.cache_hit) /
                (double)(d_cache.stat.cache_hit + d_cache.stat.cache_miss) * 100);
#endif
    ftl_log("upper level kvops: %0.2f\n", kvops);
    ftl_log("d_cache_hit: %lu, miss: %lu, hit_by_collision: %lu, "
            "miss_by_collision: %lu\n",
            d_cache.stat.cache_hit, d_cache.stat.cache_miss,
            d_cache.stat.cache_hit_by_collision,
            d_cache.stat.cache_miss_by_collision);
    // ftl_log("cmt_nr_cached_pages: %d, cmt_nr_cached_entries %d\n",
    // d_cache.member.nr_cached_tpages, d_cache.member.nr_cached_tentries);
    // ftl_log("hash_sign_collision: %lu\n", d_env.num_rd_data_miss_rd);
    // for (int i = 0; i < 64; ++i) {
    //     ftl_log("lun[%d]: rd: %ld, wr: %ld, er: %ld\n", i,
    //     ssd_li.stats->nr_nand_rd_lun[i], ssd_li.stats->nr_nand_wr_lun[i],
    //     ssd_li.stats->nr_nand_er_lun[i]);
    // }
    ftl_log("CMT number:%d\n", d_cache.member.lru->size);
    ftl_log("total_hit_inpage: %" PRIu64 ", total_inpage: %" PRIu64
            ", avg_hit_inpage: %.4f\n",
            total_hit_inpage, total_inpage, avg_hit_inpage);
    show_tail_latency_stats();
}

int parse_opts(int argc, char **argv, char *shortopts, struct option env_opts[]) {
    int ret = 0;
    int getopt_ret = 0;
    int option_index = 0;
    memset(&test_env, 0, sizeof(test_env));
    while ((getopt_ret = getopt_long(argc, argv, shortopts, env_opts, &option_index)) !=
           -1) {
        switch (getopt_ret) {
        case 0:
            // pool_size
            test_env.pool_size = atoi(optarg);
            break;
        case 1:
            // num_update
            test_env.num_update = atoi(optarg);
            break;
        case 2:
            // num_read
            test_env.num_read = atoi(optarg);
            break;
        case 3:
            // mapping_size_frac
            test_env.map_size_frac = atoi(optarg);
            break;
        case 4:
            // seed
            test_env.seed = atoi(optarg);
            break;
        case 5:
            // ext_mem_lat
            test_env.ext_mem_lat = atoi(optarg);
            break;
        case 6:
            // blktrace_file
            test_env.blktrace_file = optarg;
            break;
        case 7:
            // help
            ftl_log("Usage: %s [options]\n", argv[0]);
            ftl_log("Options:\n");
            ftl_log("  --blktrace_file <file>    Set the blktrace file for analysis\n");
            ftl_log("  --ext_mem_lat <latency>     Set the external memory latency in nanoseconds\n");
            ftl_log("  --map_size_frac <fraction>     Set the mapping size fraction (e.g., 0.25 for 25%%)\n");
            ftl_log("  --num_upper_level_kvops <count>     Set total upper-level KV operations for this blktrace\n");
            break;
        case 8:
            // num_upper_level_kvops
            test_env.num_upper_level_kvops = strtoull(optarg, NULL, 10);
            break;
        case '?':
            break;
        }
    }
    if (test_env.blktrace_file == NULL) {
        ret = -1;
    }
    return ret;
}

void test_blktrace(algorithm *palgo, const blktrace_file_content *blktrace, const int nr_workers, const bool is_blktrace_ramp);
void blktrace_fill_seq(algorithm *palgo, uint64_t num);
void blktrace_fill_random(algorithm *palgo, uint64_t num);
static uint64_t count_blktrace_replay_requests(const blktrace_file_content *blktrace,
                                               int nr_workers);

int main(int argc, char **argv) {
    ycsb_latency_samples = calloc(MAX_YCSB_SAMPLES, sizeof(uint64_t));
    total_latency_samples = calloc(MAX_TOTAL_SAMPLES, sizeof(uint64_t));
    gc_latency_samples = calloc(MAX_GC_SAMPLES, sizeof(uint64_t));
    // for read test
    ftl_log("hello world\n");

    // env create
    char *shortopts = "";
    timer_start_ns = clock_get_ns();
    if (-1 == parse_opts(argc, argv, shortopts, env_opts)) {
        printf("parse_opts failed\n");
        fflush(stdout);
        return -1;
    }
    extra_mem_lat = test_env.ext_mem_lat;
    float map_size_frac = test_env.map_size_frac;

    // map_size_frac = 64.0 / 256;
    map_size_frac = 1;

    // create ssd
    algorithm *palgo = &__demand;
    ssd_li.create(&ssd_li);
    palgo->create(palgo, &ssd_li);
    init_global_timer();
    if (map_size_frac < 1) {
        d_cache.env.nr_valid_tpages *= map_size_frac;
        d_cache.env.nr_valid_tentries = d_cache.env.nr_valid_tpages * EPP;
    }
    ftl_log("hash_table_size: %lu MB, cached: %lu MB\n",
            (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024,
            (uint64_t)d_cache.env.max_cached_tpages * PAGESIZE / 1024 / 1024);
    fflush(stdout);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&algo_tr, &attr, algo_thread, NULL);
    pthread_attr_t cq_cpl_attr;
    pthread_attr_init(&cq_cpl_attr);
    pthread_create(&finish_tr[0], &cq_cpl_attr, process_cq_cpl, NULL);
    pthread_create(&finish_tr[1], &cq_cpl_attr, process_cq_cpl, NULL);

    // preprocess blktrace file
    blktrace_file_content *blktrace = parse_blktrace_file(test_env.blktrace_file);
    if (!blktrace) {
        ftl_log("Failed to parse blktrace file: %s\n", test_env.blktrace_file);
        return -1;
    }
    total_blktrace_requests = count_blktrace_replay_requests(blktrace, NUM_WORKERS);
    ftl_log("loaded blktrace records: %lu, replay requests: %lu\n",
            blktrace->num_records, total_blktrace_requests);

    // load
    toggle_ssd_lat(false);
    ftl_log("start loading.\n");
    blktrace_fill_random(palgo, 68157440);
    // int ramp_time = 1;
    // for (int i = 0; i < ramp_time; i++) {
    //     test_blktrace(palgo, blktrace, NUM_WORKERS, true);
    // }
    clean_stats();

    // test
    ftl_log("start test.\n");
    iodepth = 32;
    ftl_log("start blktrace test. iodepth: %d\n", iodepth);
    toggle_ssd_lat(true);
    test_blktrace(palgo, blktrace, NUM_WORKERS, false);
    ftl_log("finish blktrace test.\n");
    fflush(stdout);
    blktrace_file_content_destroy(blktrace);
    sleep(2);
    show_stats();
    fflush(stdout);
    free(ycsb_latency_samples);
    free(total_latency_samples);
    free(gc_latency_samples);
    sleep(2);
    return 0;
}

void blktrace_fill_seq(algorithm *palgo, uint64_t num) {
    // load
    ftl_log("load workload size: %.2f GB\n", (double)num * 4 / 1024 / 1024);
    total_start_ns = clock_get_ns();
    volatile int tr_nr_ios = 0;
    for (uint64_t i = 1; i < num + 1; ++i) {
        int len = 3100;
        char in_buf[len + 1];
        sprintf(in_buf, "1234567");
        value_set *value = inf_get_valueset(in_buf, len);
#ifdef VALUE_CHECK
        if (!values[i]) {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->key.len = sprintf(w_req->key.key, "%lu", i);
        w_req->type = DATAW;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }
    usleep(2000000);
}

static uint64_t blktrace_rand64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static uint64_t blktrace_rand_bounded(uint64_t *state, uint64_t bound) {
    const uint64_t threshold = -bound % bound;
    uint64_t r;

    do {
        r = blktrace_rand64(state);
    } while (r < threshold);

    return r % bound;
}

static uint64_t *blktrace_create_shuffled_indices(uint64_t num_records,
                                                  int seed, int worker_id) {
    if (num_records == 0) {
        return NULL;
    }

    uint64_t *indices = g_malloc(sizeof(uint64_t) * num_records);
    uint64_t rand_state = (uint64_t)(uint32_t)seed;

    if (rand_state == 0) {
        rand_state = clock_get_ns();
    }
    rand_state ^= 0x9e3779b97f4a7c15ULL +
                  ((uint64_t)(uint32_t)worker_id << 32) +
                  (uint64_t)(uint32_t)worker_id;
    if (rand_state == 0) {
        rand_state = 1;
    }

    for (uint64_t i = 0; i < num_records; ++i) {
        indices[i] = i;
    }

    for (uint64_t i = num_records - 1; i > 0; --i) {
        uint64_t j = blktrace_rand_bounded(&rand_state, i + 1);
        uint64_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    return indices;
}

void blktrace_fill_random(algorithm *palgo, uint64_t num) {
    ftl_log("random load workload size: %.2f GB\n",
            (double)num * 4 / 1024 / 1024);
    total_start_ns = clock_get_ns();
    volatile int tr_nr_ios = 0;
    if (num == 0) {
        return;
    }
    uint64_t *keys = g_malloc(sizeof(uint64_t) * num);
    uint64_t seed = (uint64_t)test_env.seed;

    if (seed == 0) {
        seed = clock_get_ns();
    }
    if (seed == 0) {
        seed = 1;
    }

    for (uint64_t i = 0; i < num; ++i) {
        keys[i] = i + 1;
    }

    for (uint64_t i = num - 1; i > 0; --i) {
        uint64_t j = blktrace_rand_bounded(&seed, i + 1);
        uint64_t tmp = keys[i];
        keys[i] = keys[j];
        keys[j] = tmp;
    }

    for (uint64_t i = 0; i < num; ++i) {
        uint64_t key = keys[i];
        int len = 3100;
        char in_buf[len + 1];
        sprintf(in_buf, "1234567");
        value_set *value = inf_get_valueset(in_buf, len);
#ifdef VALUE_CHECK
        if (!values[key]) {
            values[key] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[key], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->key.len = sprintf(w_req->key.key, "%lu", key);
        w_req->type = DATAW;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }

    g_free(keys);
    usleep(2000000);
}

static uint64_t count_blktrace_replay_requests(const blktrace_file_content *blktrace,
                                               int nr_workers) {
    uint64_t total = 0;
    const uint64_t worker_num_records = blktrace->num_records / nr_workers;

    for (int worker_id = 0; worker_id < nr_workers; ++worker_id) {
        const uint64_t worker_offset_records = worker_id * worker_num_records;

        for (uint64_t i = 0; i < worker_num_records; ++i) {
            const blktrace_record *record =
                &blktrace->records[worker_offset_records + i];
            const uint64_t start_lpa = record->offset_secs / 8;
            const uint64_t end_lpa =
                (record->offset_secs + record->length_secs - 1) / 8;
            total += end_lpa - start_lpa + 1;
        }
    }

    return total;
}

void *test_blktrace_tr(void *args) {
    const test_worker_args *pargs = args;
    algorithm *palgo = pargs->palgo;
    const bool is_blktrace_ramp = pargs->is_blktrace_ramp;
    const blktrace_file_content *blktrace = pargs->blktrace;
    volatile int tr_nr_ios = 0;

    while (!(*pargs->pstart))
        ;

    uint64_t *ramp_indices = NULL;
    if (is_blktrace_ramp) {
        ramp_indices = blktrace_create_shuffled_indices(
            blktrace->num_records, pargs->seed, pargs->worker_id);
    }

    for (uint64_t i = 0; i < blktrace->num_records; ++i) {
        const uint64_t record_idx = ramp_indices ? ramp_indices[i] : i;
        const blktrace_record *record = &blktrace->records[record_idx];
        enum iotype io_type = record->io_type;
        if (is_blktrace_ramp && io_type == IO_TYPE_READ) {
            io_type = IO_TYPE_WRITE;
        }
        const int offset_secs = record->offset_secs;
        const int length_secs = record->length_secs;
        const int start_lpa = offset_secs / 8, end_lpa = (offset_secs + length_secs - 1) / 8;
        for (int lpa = start_lpa; lpa <= end_lpa; ++lpa) {
            int len = 3100; // for 4K pages
            char in_buf[len + 1];
            sprintf(in_buf, "1234567");
            value_set *value = inf_get_valueset(in_buf, len);
            uint64_t data_key = lpa;

            char value_str[len];
            memset(value_str, 0, len);

            request *req = g_malloc0(sizeof(request));

            switch (io_type) {
            case IO_TYPE_READ:
                sprintf(value_str, "R%lu", data_key);
                req->type = DATAR;
                req->key.len = sprintf(req->key.key, "%lu", data_key);
                req->h_params = NULL;
                req->params = NULL;
                req->state = ALGO_REQ_PENDING;
                req->value = value;
                req->end_req = end_request;
                req->ptr_nr_ios = &tr_nr_ios;
                break;
            case IO_TYPE_WRITE:
                sprintf(value_str, "W%lu", data_key);
                req->type = DATAW;
                req->key.len = sprintf(req->key.key, "%lu", data_key);
                // #ifdef VALUE_CHECK
                //         if (values[atoi(key)]) {
                //             sprintf(values[atoi(key)], "%s", value->value);
                //         } else {
                //             ftl_err("value[%lu] is NULL\n", i);
                //         }
                // #endif
                req->h_params = NULL;
                req->params = NULL;
                req->value = value;
                req->state = ALGO_REQ_PENDING;
                req->end_req = end_request;
                req->ptr_nr_ios = &tr_nr_ios;
                break;
            default:
                sprintf(value_str, "U%lu", data_key); // U for unknown
                printf("unknown IO type: %d", record->io_type);
                break;
            }
            submit_req(palgo, req);
        }
    }
    g_free(ramp_indices);
    usleep(2000000);

    return NULL;
}

blktrace_file_content *extract_blocktrace_for_worker(const blktrace_file_content *blktrace, const int nr_workers, const int worker_id) {
    const int worker_num_records = blktrace->num_records / nr_workers;
    const int worker_offset_records = worker_id * worker_num_records;
    blktrace_file_content *worker_blktrace = blktrace_file_content_create();
    for (int i = 0; i < worker_num_records; ++i) {
        const blktrace_record *record = &blktrace->records[worker_offset_records + i];
        blktrace_file_content_append_record(worker_blktrace, record);
    }
    return worker_blktrace;
}

void test_blktrace(algorithm *palgo, const blktrace_file_content *blktrace, const int nr_workers, const bool is_blktrace_ramp) {
    // update
    D_ENV(palgo)->num_rd_data_rd = 0;
    update_running = true;
    total_start_ns = clock_get_ns();
    pthread_t workers[nr_workers];
    volatile bool start = false;

    blktrace_file_content *worker_blktraces[nr_workers];
    test_worker_args worker_args[nr_workers];
    for (int i = 0; i < nr_workers; ++i) {
        worker_blktraces[i] = extract_blocktrace_for_worker(blktrace, nr_workers, i);
        test_worker_args args = {.palgo = palgo,
                                 .max = 0,
                                 .num = 0,
                                 .is_zipf = false,
                                 .seed = test_env.seed,
                                 .worker_id = i,
                                 .is_blktrace_ramp = is_blktrace_ramp,
                                 .blktrace = worker_blktraces[i],
                                 .pstart = &start};
        worker_args[i] = args;
        pthread_create(&workers[i], NULL, test_blktrace_tr, &worker_args[i]);
    }
    sleep(10);
    start = true;
    for (int i = 0; i < nr_workers; ++i) {
        pthread_join(workers[i], NULL);
    }
    for (int i = 0; i < nr_workers; ++i) {
        blktrace_file_content_destroy(worker_blktraces[i]);
    }
}
