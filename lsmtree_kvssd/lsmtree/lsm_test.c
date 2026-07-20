#include "lsmtree.h"
#include <time.h>
#include "../lsm_tools/skiplist.h"
#include <sys/prctl.h>
#include "../lsm_tools/rte_ring/rte_ring.h"
#include "lsm_utils.h"
#include "../lower/ssd.h"
#include "../lower/lower.h"
#include "../lsm_tools/container.h"
#include "../lsm_tools/queue.h"
#include <stdio.h>
#include <pthread.h>
#include <x86_64-linux-gnu/bits/pthreadtypes.h>
#include <unistd.h>
#include <x86_64-linux-gnu/bits/time64.h>
#include "../lsm_tools/latency_manager.h"
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* Zipfian distribution state */
static double zipfian_theta = 0.99;
static double zipfian_zetan;
static uint64_t zipfian_n;

/*
 * Latency statistics configuration
 * Note: MAX_LATENCY_US is defined in latency_manager.h
 */
extern lat_mgr Lat;
extern lsmtree LSM;
extern algorithm lsm_algo;
extern bm_env_t bm_env;

/* Thread handles for request processing and completion */
pthread_t algo_tr;
pthread_t finish_tr[2];

/* Statistics counters - use atomic operations for thread safety */
static uint64_t finished_r = 0;
static uint64_t finished_w = 0;

/* Batch processing configuration */
#define BATCH_SIZE (200000)
#define EVALUATION_FACTOR (10)
#define MIN_ELAPSED_S 1e-9
#define MAX_IOPS 1000000

/* Latency histogram arrays for local statistics (microsecond granularity) */
static uint64_t wlat_arr[MAX_LATENCY_US] = {0};
static uint64_t rlat_arr[MAX_LATENCY_US] = {0};
static uint64_t pt_wlat_arr = 0, pt_rlat_arr = 0;

/* Timing anchors for IOPS calculation */
static uint64_t rd_start_ns = 0;
static uint64_t wr_start_ns = 0;

/* IOPS accumulators */
static double read_iops = 0;
static double write_iops = 0;
static double update_iops = 0;
static uint64_t read_batch_cnt = 0;
static uint64_t write_batch_cnt = 0;
static uint64_t update_batch_cnt = 0;

bool update_running = false;

/*
 * Request processing thread
 * Dequeues requests from the ring buffer and dispatches to appropriate handlers.
 */
void *algo_thread()
{
    prctl(PR_SET_NAME, "algo_thread");
    algorithm *now_algo = &lsm_algo;

    while (1)
    {
        request *req = NULL;

        // Process new requests from the main queue
        if (ring_count(now_algo->req_q) > 0)
        {
            ring_dequeue(now_algo->req_q, (void *)&req, 1);
            if (!KEYVALCHECK(req->key))
                abort();
            switch (req->type)
            {
            case DATAR:
                now_algo->read(now_algo, req);
                break;
            case DATAW:
                now_algo->write(now_algo, req);
                break;
            default:
                break;
            }
        }
    }
}

/*
 * Submit a request to the algorithm for processing
 * Records the submission time (stime) for latency calculation
 */
void submit_req(algorithm *algo, request *req)
{
    req->stime = clock_get_ns();
    req->etime = 0;
    if (!KEYVALCHECK(req->key))
        abort();
    while (!ring_enqueue(algo->req_q, (void *)&req, 1))
        ;
}

/*
 * Completion queue processing thread
 * Monitors the finish queue and completes requests when their expected time arrives.
 * Uses a sorted queue to efficiently process requests by their completion time.
 */
void *process_cq_cpl()
{
    prctl(PR_SET_NAME, "cq_cpl_thread");
    algorithm *now_algo = &lsm_algo;
    request *req = NULL;
    algo_q *complete_q = algo_q_create();

    while (1)
    {
        uint64_t now = clock_get_ns();

        // Dequeue completed requests from the algorithm's finish queue
        if (ring_dequeue(now_algo->finish_q, (void *)&req, 1))
        {
            if (req->etime <= now)
            {
                // Request is ready to complete
                req->end_req(req);
            }
            else
            {
                // Request not yet ready - add to sorted wait queue
                algo_q_insert_sorted(complete_q, req, NULL);
            }
        }

        // Process requests from sorted queue whose time has arrived
        if (complete_q->head)
        {
            uint64_t now = clock_get_ns();
        retry:
            req = (request *)complete_q->head->payload;
            if (req->etime <= now)
            {
                // Track requests that completed significantly late (>10us overdue)
                if (now - req->etime > 10000)
                {
                    switch (req->type)
                    {
                    case DATAR:
                        Lat.complete_r_slow++;
                        break;
                    case DATAW:
                        Lat.complete_w_slow++;
                        break;
                    default:
                        break;
                    }
                }
                algo_q_dequeue(complete_q);
                req->end_req(req);
                if (complete_q->head)
                {
                    goto retry;
                }
            }
        }
    }
    return NULL;
}

/*
 * Request completion handler
 * Calculates latency, updates statistics, and reports batch metrics.
 * Latency = etime - stime (in nanoseconds, converted to microseconds)
 */
void end_request(request *req)
{
    switch (req->type)
    {
    case DATAR: {
        /* Calculate latency in microseconds */
        uint64_t curr_lat_ns = req->etime - req->stime;
        uint64_t rlat_us = curr_lat_ns / 1000;

        /* Update global latency histogram (for reporting) */
        if (rlat_us < MAX_LATENCY_US)
        {
            Lat.rlat_arr[rlat_us]++;
        }

        /* Free value set to prevent memory leak */
        if (req->value)
        {
            inf_free_valueset(&req->value);
            req->value = NULL;
        }

        /* Update atomic statistics counters */
        uint64_t finished_r_local = __atomic_add_fetch(&finished_r, 1, __ATOMIC_RELAXED);
        static uint64_t sum_rlat_ns = 0;
        uint64_t sum_rlat_ns_local = __atomic_add_fetch(&sum_rlat_ns, curr_lat_ns, __ATOMIC_RELAXED);

        /* Update local histogram for batch statistics */
        if (rlat_us < MAX_LATENCY_US)
        {
            __atomic_add_fetch(&rlat_arr[rlat_us], 1, __ATOMIC_RELAXED);
        }

        /* Update maximum latency tracking */
        uint64_t curr_max_rlat = __atomic_load_n(&pt_rlat_arr, __ATOMIC_RELAXED);
        if (rlat_us > curr_max_rlat)
        {
            __atomic_store_n(&pt_rlat_arr, rlat_us, __ATOMIC_RELAXED);
        }

        /* Report batch statistics every BATCH_SIZE completed reads */
        if (finished_r_local % BATCH_SIZE == 0 && finished_r_local > 0)
        {
            uint64_t now = clock_get_ns();
            if (rd_start_ns == 0)
            {
                rd_start_ns = req->stime;
            }

            /* Calculate elapsed time with overflow protection */
            uint64_t elapsed_ns = (now - rd_start_ns)* EVALUATION_FACTOR;
            if (elapsed_ns <= 0)
            {
                elapsed_ns = 1;
            }
            double elapsed_s = (double)elapsed_ns / 1e9;
            if (elapsed_s < MIN_ELAPSED_S)
            {
                elapsed_s = MIN_ELAPSED_S;
            }

            /* Calculate batch IOPS with cap */
            double batch_iops = (double)BATCH_SIZE / elapsed_s;
            if (batch_iops > MAX_IOPS)
            {
                batch_iops = MAX_IOPS;
            }

            read_iops += batch_iops;
            read_batch_cnt++;

            double avg_rlat_us = (double)sum_rlat_ns_local / 1000.0 / BATCH_SIZE;
            uint64_t max_rlat_us = __atomic_load_n(&pt_rlat_arr, __ATOMIC_RELAXED);

            printf("[READ STAT] %lu th read end. batch_iops: %.2f, avg_lat: %.2f us, max_lat: %lu us, total_avg_iops: %.2f\n",
                   finished_r_local, batch_iops, avg_rlat_us, max_rlat_us,
                   read_batch_cnt > 0 ? (read_iops / read_batch_cnt) : 0);

            fflush(stdout);
            fflush(stderr);

            __atomic_store_n(&sum_rlat_ns, 0, __ATOMIC_RELAXED);
            rd_start_ns = now;
        }
        break;
    }
    case DATAW: {
        /* Calculate latency in microseconds */
        uint64_t curr_lat_ns = req->etime - req->stime;
        uint64_t wlat_us = curr_lat_ns / 1000;

        /* Update global latency histogram */
        if (wlat_us < MAX_LATENCY_US)
        {
            Lat.wlat_arr[wlat_us]++;
        }

        /* Free value set */
        if (req->value)
        {
            inf_free_valueset(&req->value);
            req->value = NULL;
        }

        /* Update atomic counters */
        uint64_t finished_w_local = __atomic_add_fetch(&finished_w, 1, __ATOMIC_RELAXED);
        static uint64_t sum_wlat_ns = 0;
        uint64_t sum_wlat_ns_local = __atomic_add_fetch(&sum_wlat_ns, curr_lat_ns, __ATOMIC_RELAXED);

        /* Update local histogram */
        if (wlat_us < MAX_LATENCY_US)
        {
            __atomic_add_fetch(&wlat_arr[wlat_us], 1, __ATOMIC_RELAXED);
        }

        /* Update max latency tracking */
        uint64_t curr_max_wlat = __atomic_load_n(&pt_wlat_arr, __ATOMIC_RELAXED);
        if (wlat_us > curr_max_wlat)
        {
            __atomic_store_n(&pt_wlat_arr, wlat_us, __ATOMIC_RELAXED);
        }

        /* Report batch statistics every BATCH_SIZE completed writes */
        if (finished_w_local % BATCH_SIZE == 0 && finished_w_local > 0)
        {
            uint64_t now = clock_get_ns();
            if (wr_start_ns == 0)
            {
                wr_start_ns = req->stime;
            }

            uint64_t elapsed_ns = (now - wr_start_ns)* EVALUATION_FACTOR;
            if (elapsed_ns <= 0)
            {
                elapsed_ns = 1;
            }
            double elapsed_s = (double)elapsed_ns / 1e9;
            if (elapsed_s < MIN_ELAPSED_S)
            {
                elapsed_s = MIN_ELAPSED_S;
            }

            double batch_iops = (double)BATCH_SIZE / elapsed_s;
            if (batch_iops > MAX_IOPS)
            {
                batch_iops = MAX_IOPS;
            }

            /* Track write vs update separately */
            if (update_running)
            {
                update_iops += batch_iops;
                update_batch_cnt++;
            }
            else
            {
                write_iops += batch_iops;
                write_batch_cnt++;
            }

            double avg_wlat_us = (double)(sum_wlat_ns_local / BATCH_SIZE) / 1000.0;
            uint64_t max_wlat_us = __atomic_load_n(&pt_wlat_arr, __ATOMIC_RELAXED);

            if (update_running)
            {
                printf("[UPDATE STAT] %lu th update end. batch_iops: %.2f, avg_lat: %.2f us, max_lat: %lu us, total_avg_iops: %.2f\n",
                       finished_w_local, batch_iops, avg_wlat_us, max_wlat_us,
                       update_batch_cnt > 0 ? (update_iops / update_batch_cnt) : 0);
            }
            else
            {
                printf("[WRITE STAT] %lu th write end. batch_iops: %.2f, avg_lat: %.2f us, max_lat: %lu us, total_avg_iops: %.2f\n",
                       finished_w_local, batch_iops, avg_wlat_us, max_wlat_us,
                       write_batch_cnt > 0 ? (write_iops / write_batch_cnt) : 0);
            }

            fflush(stdout);
            fflush(stderr);

            __atomic_store_n(&sum_wlat_ns, 0, __ATOMIC_RELAXED);
            wr_start_ns = now;
        }
        break;
    }
    default:
        break;
    }
}


/*
 * Load test function - performs sequential writes to populate the LSM-tree
 * Generates 'num' key-value pairs and submits them as write requests
 */
void lsm_test_load(algorithm *algo, uint64_t num)
{
    uint64_t j = 1;
    srand((unsigned)time(NULL));
    for (uint64_t i = 1; i < num + 1; ++i)
    {
        j = (rand() % (num)) + 1;
        int len = PAGESIZE;
        value_set *value = inf_get_valueset("1234567\0", len);
        request *w_req = malloc(sizeof(request));
        w_req->key.key = malloc(MAXKEYLENGTH);
        w_req->key.len = sprintf(w_req->key.key, "%lu", i);
        w_req->value = value;
        w_req->type = DATAW;
        w_req->is_compacting = false;
        w_req->end_req = end_request;
        if (!KEYVALCHECK(w_req->key))
            abort();
        submit_req(algo, w_req);
    }
    sleep(10);
}

/* Read operation statistics counter (used by lsmtree.c) */
int read_type_cnt[4] = {0};

/* Zipfian distribution helper functions */
static double zeta(uint64_t n, double theta)
{
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; i++)
    {
        sum += 1.0 / pow((double)i, theta);
    }
    return sum;
}

/*
 * Initialize Zipfian distribution
 */
static void zipfian_init(uint64_t n, double theta)
{
    zipfian_n = n;
    zipfian_theta = theta;
    zipfian_zetan = zeta(n, theta);
}

/*
 * Generate Zipfian distributed random number in range [1, n]
 * Uses algorithm from "Quickly Generating Billion-Record Synthetic Databases"
 */
static uint64_t zipfian_rand(void)
{
    double u = (double)rand() / (double)RAND_MAX;
    double uz = u * zipfian_zetan;
    uint64_t v = (uint64_t)(zipfian_n * pow(1 - uz, 1.0 / (1.0 - zipfian_theta)) + 1);
    if (v < 1 || v > zipfian_n)
    {
        v = 1;
    }
    return v;
}

/*
 * Submit a write request (for YCSB workloads)
 */
static void submit_write_req(algorithm *algo, uint64_t key_num, uint32_t seed)
{
    int len = PAGESIZE;
    value_set *value = inf_get_valueset("1234567\0", len);
    request *w_req = malloc(sizeof(request));
    w_req->key.key = malloc(MAXKEYLENGTH);
    w_req->key.len = sprintf(w_req->key.key, "%lu", key_num);
    w_req->value = value;
    w_req->type = DATAW;
    w_req->is_compacting = false;
    w_req->end_req = end_request;
    if (!KEYVALCHECK(w_req->key))
        abort();
    submit_req(algo, w_req);
}

/*
 * Submit a read request (for YCSB workloads)
 */
static void submit_read_req(algorithm *algo, uint64_t key_num)
{
    request *r_req = malloc(sizeof(request));
    r_req->key.key = malloc(MAXKEYLENGTH);
    r_req->key.len = sprintf(r_req->key.key, "%lu", key_num);
    r_req->params = NULL;
    r_req->value = NULL;
    r_req->type = DATAR;
    r_req->end_req = end_request;
    if (!KEYVALCHECK(r_req->key))
        abort();
    submit_req(algo, r_req);
}

/*
 * YCSB Workload A: 50% Read, 50% Update (Zipfian)
 */
void lsm_test_ycsb_a(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        if (rand() % 2 == 0)
        {
            submit_read_req(algo, rndkey);
        }
        else
        {
            submit_write_req(algo, rndkey, seed);
        }
    }
}

/*
 * YCSB Workload B: 95% Read, 5% Update (Zipfian)
 */
void lsm_test_ycsb_b(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        if (rand() % 20 != 0)
        {
            submit_read_req(algo, rndkey);
        }
        else
        {
            submit_write_req(algo, rndkey, seed);
        }
    }
}

/*
 * YCSB Workload C: 100% Read (Sequential scan)
 */
void lsm_test_ycsb_c(algorithm *algo, uint64_t num)
{
    for (uint64_t i = 1; i <= num; i++)
    {
        submit_read_req(algo, i);
    }
}

/*
 * YCSB Workload D: 95% Read, 5% Insert (Read latest)
 */
void lsm_test_ycsb_d(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        if (rand() % 20 != 0)
        {
            submit_read_req(algo, rndkey);
        }
        else
        {
            uint64_t newkey = num + (rand() % num) + 1;
            submit_write_req(algo, newkey, seed);
        }
    }
}

/*
 * YCSB Workload E: 95% Insert, 5% Read (Short scans)
 */
void lsm_test_ycsb_e(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);

    for (uint64_t i = 1; i <= num; i++)
    {
        if (rand() % 20 != 0)
        {
            uint64_t newkey = num + i;
            submit_write_req(algo, newkey, seed);
        }
        else
        {
            uint64_t scan_start = rand() % num;
            for (int j = 0; j < 10 && scan_start + j <= num; j++)
            {
                submit_read_req(algo, scan_start + j);
            }
        }
    }
}

/*
 * YCSB Workload F: 50% Read, 50% Read-Modify-Write
 */
void lsm_test_ycsb_f(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        if (rand() % 2 == 0)
        {
            submit_read_req(algo, rndkey);
        }
        else
        {
            submit_read_req(algo, rndkey);
            submit_write_req(algo, rndkey, seed);
        }
    }
}

/*
 * Random read test (uniform distribution)
 */
void lsm_test_random_read(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = rand() % num + 1;
        submit_read_req(algo, rndkey);
    }
}

/*
 * Zipfian read test
 */
void lsm_test_zipfian_read(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        submit_read_req(algo, rndkey);
    }
}

/*
 * Zipfian update test
 */
void lsm_test_zipfian_update(algorithm *algo, uint64_t num)
{
    uint32_t seed = (uint32_t)time(NULL);
    srand(seed);
    zipfian_init(num, 0.99);
    update_running = true;

    for (uint64_t i = 1; i <= num; i++)
    {
        uint64_t rndkey = zipfian_rand();
        submit_write_req(algo, rndkey, seed);
    }
}

/* Forward declarations for workload functions */
void lsm_test_random_read(algorithm *algo, uint64_t num);
void lsm_test_zipfian_read(algorithm *algo, uint64_t num);
void lsm_test_zipfian_update(algorithm *algo, uint64_t num);
void lsm_test_ycsb_a(algorithm *algo, uint64_t num);
void lsm_test_ycsb_b(algorithm *algo, uint64_t num);
void lsm_test_ycsb_c(algorithm *algo, uint64_t num);
void lsm_test_ycsb_d(algorithm *algo, uint64_t num);
void lsm_test_ycsb_e(algorithm *algo, uint64_t num);
void lsm_test_ycsb_f(algorithm *algo, uint64_t num);

/*
 * Print usage information
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --workload <name>   Workload type: random_read, zipfian_read, zipfian_update, ycsb_a, ycsb_b, ycsb_c, ycsb_d, ycsb_e, ycsb_f\n");
    printf("  --num <N>           Number of KV pairs (default: 8000000)\n");
    printf("  --load <N>          Number of KV pairs for load phase (default: same as --num)\n");
    printf("  --help              Show this help message\n");
    printf("\nWorkload descriptions:\n");
    printf("  random_read     : Uniform random read\n");
    printf("  zipfian_read    : Zipfian distributed read\n");
    printf("  zipfian_update  : Zipfian distributed update\n");
    printf("  ycsb_a          : 50%% Read, 50%% Update (Zipfian)\n");
    printf("  ycsb_b          : 95%% Read, 5%% Update (Zipfian)\n");
    printf("  ycsb_c          : 100%% Read (Sequential)\n");
    printf("  ycsb_d          : 95%% Read, 5%% Insert (Read latest)\n");
    printf("  ycsb_e          : 95%% Insert, 5%% Read (Short scans)\n");
    printf("  ycsb_f          : 50%% Read, 50%% Read-Modify-Write\n");
}

/*
 * Main entry point - initializes the LSM-tree simulator and runs tests
 */
int main(int argc, char *argv[])
{
    /* Default configuration */
    const char *workload = "random_read";
    uint64_t num_kv = 8000000;
    uint64_t load_kv = 0;  /* 0 means same as num_kv */

    /* Parse command line arguments */
    static struct option long_options[] = {
        {"workload", required_argument, 0, 'w'},
        {"num",      required_argument, 0, 'n'},
        {"load",     required_argument, 0, 'l'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "w:n:l:h", long_options, &option_index)) != -1)
    {
        switch (c)
        {
        case 'w':
            workload = optarg;
            break;
        case 'n':
            num_kv = atoll(optarg);
            break;
        case 'l':
            load_kv = atoll(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Set load_kv to num_kv if not specified */
    if (load_kv == 0)
    {
        load_kv = num_kv;
    }

    printf("========================================\n");
    printf(" LSM-tree Test Configuration\n");
    printf("========================================\n");
    printf(" Workload:     %s\n", workload);
    printf(" Load KV:      %lu\n", load_kv);
    printf(" Test KV:      %lu\n", num_kv);
    printf("========================================\n\n");

    /* Initialize global LSM-tree structure with algorithm */
    LSM.algo = &lsm_algo;

    /* Initialize latency manager */
    lat_init();

    /* Create and initialize the algorithm (LSM-tree) */
    LSM.algo->create(LSM.algo, LSM.algo->li);

    /* Create worker threads */
    pthread_attr_t algo_attr;
    pthread_attr_init(&algo_attr);
    pthread_create(&algo_tr, &algo_attr, algo_thread, NULL);

    pthread_attr_t cq_cpl_attr;
    pthread_attr_init(&cq_cpl_attr);
    pthread_create(&finish_tr[0], &cq_cpl_attr, process_cq_cpl, NULL);
    pthread_create(&finish_tr[1], &cq_cpl_attr, process_cq_cpl, NULL);

    /* Run workload based on type */
    printf("[INFO] Starting workload: %s\n", workload);
    printf("[INFO] Load phase: %lu KV pairs\n", load_kv);

    /* Load phase */
    lsm_test_load(LSM.algo, load_kv);
    sleep(5);

    printf("[INFO] Load phase completed\n");
    printf("[INFO] Test phase: %lu KV pairs\n", num_kv);

    /* Run specific workload */
    if (strcmp(workload, "random_read") == 0)
    {
        lsm_test_random_read(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "zipfian_read") == 0)
    {
        lsm_test_zipfian_read(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "zipfian_update") == 0)
    {
        lsm_test_zipfian_update(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_a") == 0)
    {
        lsm_test_ycsb_a(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_b") == 0)
    {
        lsm_test_ycsb_b(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_c") == 0)
    {
        lsm_test_ycsb_c(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_d") == 0)
    {
        lsm_test_ycsb_d(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_e") == 0)
    {
        lsm_test_ycsb_e(LSM.algo, num_kv);
    }
    else if (strcmp(workload, "ycsb_f") == 0)
    {
        lsm_test_ycsb_f(LSM.algo, num_kv);
    }
    else
    {
        printf("[ERROR] Unknown workload: %s\n", workload);
        printf("Run with --help for available workloads\n");
        return 1;
    }

    /* Wait for operations to complete */
    sleep(5);

    /* Print final latency statistics */
    printf("\n========================================\n");
    printf(" Final Results\n");
    printf("========================================\n");
    mix_lat_print();
    printf("Skiplist node: %ld\n", LSM.memtable->size);
    LSM.lop->print_level_summary();

    return 0;
}