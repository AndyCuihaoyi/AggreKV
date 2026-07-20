// 标准头文件
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

// 第三方头文件
#include <glib-2.0/glib.h>

// 项目头文件
#include "../lower/lower.h"
#include "../tools/random/zipf.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/valueset.h"
#include "algo_queue.h"
#include "cache.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_pg.h"
#include "dftl_settings.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "dftl_wb.h"
#include "request.h"

#define NUM_ITEMS (44000000)
#define NUM_UPDATES (44000000)
#define NUM_WORKERS (7)

/* 默认 Zipf 参数（可用 --zipf_alpha 覆盖） */
double READ_ALPHA = 0.99;
double UPDATE_ALPHA = 0.99;

/* 键长配置（--variable_keylen / --keylen_mean） */
bool ENABLE_VARIABLE_KEYLEN = false;
int KEYLEN_MEAN = 16;   // 键长均值（16/32/64B三档）
int read_dist_mode = 0;  // 0=zipfian (default), 1=uniform
int update_dist_mode = 0; // 0=zipfian (default), 1=uniform

// 键长标准差自动计算为均值的15%
#define KEYLEN_STD_PERCENT 0.15

// 高斯分布随机数生成器
static double gaussian_random(double mean, double stddev, unsigned int *seed) {
  // Box-Muller算法
  double u1 = (double)rand_r(seed) / RAND_MAX;
  double u2 = (double)rand_r(seed) / RAND_MAX;
  double z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
  return z0 * stddev + mean;
}

/* 由 rndkey 确定性得到键长（高斯分布），同一 rndkey 读写长度一致 */
static int keylen_for_rndkey(uint64_t rndkey) {
  unsigned int s;
  double len;
  int keylen;
  int keylen_std;

  if (!ENABLE_VARIABLE_KEYLEN)
    return -1;

  // 标准差为均值的15%
  keylen_std = (int)(KEYLEN_MEAN * KEYLEN_STD_PERCENT);

  s = (unsigned)(rndkey ^ (rndkey >> 32));
  if (s == 0)
    s = 1;
  len = gaussian_random(KEYLEN_MEAN, keylen_std, &s);
  keylen = (int)round(len);
  if (keylen < 1)
    keylen = 1;
  if (keylen > MAXKEYSIZE)
    keylen = MAXKEYSIZE;
  return keylen;
}

/* 生成指定长度的键：内容仅由 rndkey 决定，与 load/YCSB 路径一致。
 * 返回实际键长；当目标 keylen 小于 %016lu 前缀时保留完整前缀，避免截断碰撞。 */
static int generate_key(char *buf, int keylen, uint64_t rndkey) {
  int n;
  int i;

  if (keylen == -1) {
    return sprintf(buf, "%lu", rndkey);
  }

  n = sprintf(buf, "%016lu", rndkey);
  if (keylen <= n) {
    return n;
  }
  for (i = n; i < keylen; i++)
    buf[i] = 'a' + (int)((rndkey + (uint64_t)i * 17) % 26);
  buf[keylen] = '\0';
  return keylen;
}

static void fill_request_key(KEYT *key, uint64_t rndkey) {
  int keylen = keylen_for_rndkey(rndkey);
  key->len = (uint8_t)generate_key(key->key, keylen, rndkey);
}

volatile bool thread_ended[NUM_WORKERS] = {false};
int read_worker_cnt = 0;
bool full_worker = true;
// #define VALUE_CHECK

bool ycsb_running = false;
bool update_running = false;
bool ycsb_rmw_running = false;
volatile int gc_inflight = 0;
// YCSB全局统计变量（不分读写）
static uint64_t ycsb_total_ops = 0;      // YCSB总操作数（原子）
static uint64_t ycsb_total_lat_ns = 0;   // YCSB总延迟（纳秒，原子）
static uint64_t ycsb_batch_start_ns = 0; // YCSB批次开始时间
static double ycsb_total_iops = 0;       // YCSB总IOPS累加值
static int ycsb_batch_cnt = 0;           // YCSB批次计数
static uint64_t ycsb_max_lat_us = 0;     // YCSB最大延迟（微秒）

void *test_rmw_tr(void *args);
static void *test_scan_tr(void *args);
static void *test_insert_latest_tr(void *args);
// YCSB各负载独立函数声明
void test_ycsb_a(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_b(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_c(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_d(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_e(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_f(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void ycsb_run_workload(char workload, algorithm *palgo, uint64_t pool_size,
                       uint64_t num_ops, int nr_workers, int seed);

#define MAX_READ_SAMPLES (67200000) // 按需调整，足够存储所有读请求延迟
static uint64_t *read_latency_samples = NULL;
static uint64_t read_sample_count = 0; // 实际收集的读延迟样本数
#define MAX_WRITE_SAMPLES (67200000)
static uint64_t *write_latency_samples = NULL;
static uint64_t write_sample_count = 0;
#define MAX_YCSB_SAMPLES (67200000)
static uint64_t ycsb_sample_count = 0;
static uint64_t *ycsb_latency_samples = NULL;
#define MAX_GC_SAMPLES (9000000)
static uint64_t *gc_latency_samples = NULL;
static uint64_t gc_sample_count = 0;

uint64_t ycsb_total_batch_avg_lat_us = 0;
uint64_t *shuffle_map = NULL;
extern lower_info ssd_li;
int iodepth = 1;
pthread_spinlock_t global_inner_timer_lock;
struct req_inner_timer global_inner_timer[INNER_TIMER_SIZE] = {0};

void submit_req(algorithm *palgo, request *req);

static uint64_t finished_r = 0;
static uint64_t finished_w = 0;
static uint64_t wrong_value_cnt = 0;
static uint64_t total_hit_inpage = 0;
static uint64_t total_inpage = 0;
static float avg_hit_inpage = 0.0;
#ifdef VALUE_CHECK
char *values[NUM_ITEMS] = {0};
#endif
#define BATCH_SIZE (200000)
#define MAX_LATENCY_US 100000000 // 10s

static uint64_t wlat_arr[MAX_LATENCY_US] = {0};
static uint64_t rlat_arr[MAX_LATENCY_US] = {0};
static uint64_t pt_wlat_arr = 0, pt_rlat_arr = 0;

static uint64_t rd_start_ns = 0;
static uint64_t wr_start_ns = 0;

static uint64_t complete_r_slow = 0;
static uint64_t complete_w_slow = 0;

static double read_iops = 0;
static double write_iops = 0;
static double update_iops = 0;
static double gc_iops = 0;
static uint64_t read_batch_cnt = 0;
static uint64_t write_batch_cnt = 0;
static uint64_t update_batch_cnt = 0;
static uint64_t gc_batch_cnt = 0;

int compare_uint32(const void *a, const void *b);

// 再实现函数
int compare_uint32(const void *a, const void *b) {
  const uint32_t *val_a = (const uint32_t *)a;
  const uint32_t *val_b = (const uint32_t *)b;
  if (*val_a < *val_b)
    return -1;
  if (*val_a > *val_b)
    return 1;
  return 0;
}

void *end_request(request *req) {
  // req->etime = clock_get_ns();
  if (ycsb_running) {
    uint64_t ycsb_ops_local = g_atomic_pointer_add(&ycsb_total_ops, 1);

    uint64_t curr_lat_ns = req->etime - req->stime;
    uint64_t curr_lat_us = curr_lat_ns / 1000;
    uint64_t ycsb_total_lat_local =
        __atomic_add_fetch(&ycsb_total_lat_ns, curr_lat_ns, __ATOMIC_RELAXED);
    if (ycsb_sample_count < MAX_READ_SAMPLES) {
      read_latency_samples[ycsb_sample_count++] = curr_lat_us;
    }
    uint64_t curr_max_lat = g_atomic_pointer_get(&ycsb_max_lat_us);
    if (curr_lat_us > curr_max_lat) {
      g_atomic_pointer_set(&ycsb_max_lat_us, curr_lat_us);
    }

    if (ycsb_sample_count < MAX_YCSB_SAMPLES) {
      ycsb_latency_samples[ycsb_sample_count++] = curr_lat_us;
    }

    if (ycsb_ops_local == 1) {
      ycsb_batch_start_ns = req->stime;
    }

    if (ycsb_ops_local % BATCH_SIZE == 0 && ycsb_ops_local > 0) {
      uint64_t now = clock_get_ns();
      double elapsed_s = (now - ycsb_batch_start_ns) / 1e9;
      double batch_iops = (double)BATCH_SIZE / elapsed_s;
      if (ycsb_rmw_running)
        batch_iops = batch_iops / 2;
      ycsb_total_iops += batch_iops;
      ycsb_batch_cnt++;
      uint64_t avg_lat_us = (ycsb_total_lat_local / BATCH_SIZE) / 1000;
      __atomic_add_fetch(&ycsb_total_batch_avg_lat_us, avg_lat_us,
                         __ATOMIC_RELAXED);
      ftl_log("[YCSB GLOBAL] %lu th op end. total_iops: %.2f, batch_iops: "
              "%.2f, avg_lat: %lu us, max_lat: %lu us\n",
              ycsb_ops_local, ycsb_total_iops / ycsb_batch_cnt, batch_iops,
              avg_lat_us, g_atomic_pointer_get(&ycsb_max_lat_us));

      g_atomic_pointer_set(&ycsb_total_lat_ns, 0);
      ycsb_batch_start_ns = now;
      fflush(stdout);
      fflush(stderr);
    }
  }

  switch (req->type) {
  case DATAR:
    if (req->state == ALGO_REQ_NOT_FOUND) {
      ftl_log("%lu th read request end. key: %s, value not found\n",
              ++finished_r, req->key.key);
    } else {
      // finished_r++;
      uint64_t finished_r_local = g_atomic_pointer_add(&finished_r, 1);
      static uint64_t sum_rlat_ns = 0;
      uint64_t sum_rlat_ns_local =
          g_atomic_pointer_add(&sum_rlat_ns, (req->etime - req->stime));

      uint64_t latency_us = (req->etime - req->stime) / 1000;
      if (read_sample_count < MAX_READ_SAMPLES) {
        read_latency_samples[read_sample_count++] = latency_us;
      }
      if (finished_r_local == 16000000) {
        struct cmt_struct *target_cmt;
        NODE *current_node = lru_get_target_node(d_cache.member.lru, NULL);

        if (current_node == NULL) {
          ftl_warn("LRU list is empty, no node to iterate\n");
        }
        while (current_node != NULL) {
          target_cmt = (struct cmt_struct *)current_node->DATA;

          if (target_cmt != NULL && target_cmt->pt != NULL) {
            total_inpage++;
            total_hit_inpage += target_cmt->page_heat_sum;
          }
          current_node = lru_get_target_node(NULL, current_node);
        }
        avg_hit_inpage =
            total_inpage ? (float)total_hit_inpage / (float)total_inpage : 0.0f;
      }
      if ((++finished_r_local) % BATCH_SIZE == 0) {
        uint64_t now = clock_get_ns();
        double elapsed = (now - rd_start_ns) / 1e9;
        if (full_worker) {
          for (int i = 0; i < NUM_WORKERS; ++i) {
            if (thread_ended[i]) {
              ftl_log("Thread %d ended !\n", i);
              full_worker = false;
            }
          }
        }
        if (full_worker) {
          read_iops += (double)BATCH_SIZE / elapsed;
          read_batch_cnt++;
        }
        rd_start_ns = now;
        ftl_log("%lu th read request end. key: %s, iops: %lf, avg_lat: %ld, "
                "slow: %ld\n",
                finished_r_local, req->key.key, (double)BATCH_SIZE / elapsed,
                sum_rlat_ns_local / BATCH_SIZE, complete_r_slow);
#ifdef HOT_CMT
        cmt_heat_log_batch_period(&d_cache, "read");
#endif
#ifdef ADAPTIVE_MEM
        adaptive_log_rebalance_period(&d_cache, "read");
#endif
        g_atomic_pointer_set(&sum_rlat_ns, 0);
        complete_r_slow = 0;
        fflush(stdout);
        fflush(stderr);
      }
      uint64_t rlat_us = (req->etime - req->stime) / 1000;
      g_atomic_pointer_add(&rlat_arr[rlat_us], 1);
      if (rlat_us > pt_rlat_arr) {
        g_atomic_pointer_set(&pt_rlat_arr, rlat_us);
      }
    }
    if (req->value) {
      inf_free_valueset(&req->value);
    }
    break;
  case DATAW: {
    uint64_t finished_w_local = g_atomic_pointer_add(&finished_w, 1);
    static uint64_t sum_wlat_ns = 0;
    uint64_t latency = req->etime - req->stime;
    uint64_t latency_us = latency / 1000;

    uint64_t sum_wlat_ns_local =
        __atomic_add_fetch(&sum_wlat_ns, latency, __ATOMIC_RELAXED);
    if (write_sample_count < MAX_WRITE_SAMPLES && update_running == true) {
      write_latency_samples[write_sample_count++] = latency_us;
    }
    if (gc_sample_count < MAX_GC_SAMPLES &&
        __atomic_load_n(&gc_inflight, __ATOMIC_RELAXED) > 0) {
      gc_latency_samples[gc_sample_count++] = latency_us;
    }
    if ((++finished_w_local) % BATCH_SIZE == 0) {
      // ftl_log("\033[1;32m%lu\033[0m th write request end.\n", finished_w);
      uint64_t now = clock_get_ns();
      double elapsed = (now - wr_start_ns) / 1e9;
      if (update_running) {
        update_iops += (double)BATCH_SIZE / elapsed;
        update_batch_cnt++;
        wr_start_ns = now;
        ftl_log("%lu th update request end, iops: %lf, avg_lat: %ld, slow: "
                "%ld\n",
                finished_w_local, (double)BATCH_SIZE / elapsed,
                sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
#ifdef HOT_CMT
        cmt_heat_log_batch_period(&d_cache, "update");
#endif
#ifdef ADAPTIVE_MEM
        adaptive_log_rebalance_period(&d_cache, "update");
#endif
      } else if (__atomic_load_n(&gc_inflight, __ATOMIC_RELAXED) > 0) {
        gc_iops += (double)BATCH_SIZE / elapsed;
        gc_batch_cnt++;
        wr_start_ns = now;
        ftl_log("%lu th gc write request end, iops: %lf, avg_lat: %ld, slow: "
                "%ld\n",
                finished_w_local, (double)BATCH_SIZE / elapsed,
                sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
#ifdef HOT_CMT
        cmt_heat_log_batch_period(&d_cache, "gc");
#endif
#ifdef ADAPTIVE_MEM
        adaptive_log_rebalance_period(&d_cache, "gc");
#endif
      } else {
        write_iops += (double)BATCH_SIZE / elapsed;
        write_batch_cnt++;
        wr_start_ns = now;
        ftl_log(
            "%lu th write request end, iops: %lf, avg_lat: %ld, slow: %ld\n",
            finished_w_local, (double)BATCH_SIZE / elapsed,
            sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
#ifdef HOT_CMT
        cmt_heat_log_batch_period(&d_cache, "write");
#endif
#ifdef ADAPTIVE_MEM
        adaptive_log_rebalance_period(&d_cache, "write");
#endif
      }
      g_atomic_pointer_set(&sum_wlat_ns, 0);
      complete_w_slow = 0;
      fflush(stdout);
      fflush(stderr);
    }
    uint64_t wlat_us = (req->etime - req->stime) / 1000;
    g_atomic_pointer_add(&wlat_arr[wlat_us], 1);
    if (wlat_us > pt_wlat_arr) {
      g_atomic_pointer_set(&pt_wlat_arr, wlat_us);
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

pthread_spinlock_t submit_lock;

void *algo_thread() {
  prctl(PR_SET_NAME, "algo_thread");
  algorithm *palgo = &__demand;
  pthread_spin_init(&submit_lock, PTHREAD_PROCESS_PRIVATE);
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

pthread_t algo_tr;
pthread_t finish_tr[2];

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
          switch (req->type) {
          case DATAR:
            complete_r_slow++;
            break;
          case DATAW:
            complete_w_slow++;
            break;
          default:
            break;
          }
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
  wr_start_ns = clock_get_ns();
  uint64_t max = num;
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
    fill_request_key(&w_req->key, i);
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

// 新增：test_load的线程包装函数（内部直接调用原test_load，适配并行启动）
void *test_load_wrapper_tr(void *args) {
  struct {
    algorithm *palgo;
    uint64_t num;
    volatile bool *pstart;
  } *pargs = args;
  algorithm *palgo = pargs->palgo;
  uint64_t num = pargs->num;

  // 等待全局启动信号（和读/扫描线程同步）
  while (!(*pargs->pstart))
    ;

  // 直接调用你原有的test_load函数
  test_load(palgo, num);

  return NULL;
}

/* YCSB D/E 插入新 key：从 base 开始写 num 条，避免覆盖已加载的旧 key */
void *test_insert_latest_tr(void *args) {
  struct {
    algorithm *palgo;
    uint64_t base; // 起始 key（pool_size + 1 或更高）
    uint64_t num;
    volatile bool *pstart;
  } *pargs = args;
  algorithm *palgo = pargs->palgo;
  uint64_t base = pargs->base;
  uint64_t num = pargs->num;
  volatile int tr_nr_ios = 0;

  while (!(*pargs->pstart))
    ;

  for (uint64_t i = 0; i < num; i++) {
    value_set *value = inf_get_valueset("1234567\0", 8);
    request *w_req = g_malloc0(sizeof(request));
    fill_request_key(&w_req->key, base + i);
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
  return NULL;
}

void *test_update_tr(void *args) {
  // update
  struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } *pargs = args;
  algorithm *palgo = pargs->palgo;
  uint64_t max = pargs->max;
  uint64_t num = pargs->num;
  bool is_zipf = pargs->is_zipf;
  int seed = pargs->seed;
  volatile int tr_nr_ios = 0;

  struct zipf_state zs;
  unsigned int local_seed = seed + pthread_self();
  srand(local_seed);
  zipf_init(&zs, max - 1, UPDATE_ALPHA, -1, seed);
  if (shuffle_map)
    zipf_use_shuffle_map(&zs, shuffle_map);
  while (!(*pargs->pstart))
    ;
  for (uint64_t i = 0; i < num; ++i) {
    int len = 8;
    value_set *value = inf_get_valueset("1234567\0", len);
    uint64_t rndkey;
    if (is_zipf)
      rndkey = zipf_next(&zs) + 1;
    else
      rndkey = rand_r(&local_seed) % (max - 1) + 1;

    request *w_req = g_malloc0(sizeof(request));
    w_req->type = DATAW;
    fill_request_key(&w_req->key, rndkey);
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
  wr_start_ns = clock_get_ns();
  pthread_t workers[nr_workers];
  volatile bool start = false;
  struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } args = {.palgo = palgo,
            .max = max,
            .num = num,
            .is_zipf = is_zipf,
            .seed = seed,
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
  int worker_id = g_atomic_int_add(&read_worker_cnt, 1);
  prctl(PR_SET_NAME, "read_worker");

  struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } *pargs = args;

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
      zipf_use_shuffle_map(&zs, shuffle_map);
  }

  srand(seed);

  unsigned int local_seed = seed + worker_id;
  while (!(*pargs->pstart))
    ;
  for (uint64_t i = 0; i < num; ++i) {
    uint64_t rndkey;
    if (is_seq) {
      rndkey = (uint64_t)worker_id * num + i + 1;
    } else if (is_zipf) {
      rndkey = zipf_next(&zs) + 1;
    } else {
      rndkey = rand_r(&local_seed) % (max - 1) + 1;
    }

    value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
    request *r_req = g_malloc0(sizeof(request));
    r_req->type = DATAR;
    fill_request_key(&r_req->key, rndkey);
    r_req->h_params = NULL;
    r_req->params = NULL;
    r_req->state = ALGO_REQ_PENDING;
    r_req->value = r_value;
    r_req->end_req = end_request;
    r_req->ptr_nr_ios = &tr_nr_ios;
    submit_req(palgo, r_req);
  }

  if (worker_id >= 0 && worker_id < NUM_WORKERS)
    thread_ended[worker_id] = true;
  usleep(2000000);
  return NULL;
}

static void reset_read_worker_state(void) {
  read_worker_cnt = 0;
  full_worker = true;
  for (int i = 0; i < NUM_WORKERS; ++i)
    thread_ended[i] = false;
  read_iops = 0;
  read_batch_cnt = 0;
}

void test_read(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf,
               bool is_seq, int seed, int nr_workers) {
  // read
  reset_read_worker_state();
  ftl_log("read workload size: %.2f GB\n",
          (double)num * NUM_WORKERS * PIECE / G);
  rd_start_ns = clock_get_ns();
  pthread_t workers[nr_workers];
  volatile bool start = false;
  struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } args = {.palgo = palgo,
            .max = max,
            .num = num,
            .is_zipf = is_zipf,
            .is_seq = is_seq,
            .seed = seed,
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
  req->stime = clock_get_ns();
  while (!ring_enqueue(palgo->req_q, (void *)&req, 1))
    ;
  ftl_assert((*req->ptr_nr_ios) <= iodepth);
  // pthread_spin_unlock(&submit_lock);
}

void show_latency_stats() {
  uint64_t avg_rlat = 0, avg_wlat = 0;
  uint64_t nr_rd = 0, nr_wr = 0;
  for (uint64_t i = 0; i <= pt_rlat_arr; i++) {
    if (rlat_arr[i]) {
      // ftl_log("read latency: %lu us, count: %lu\n", i, rlat_arr[i]);
      nr_rd += rlat_arr[i];
      avg_rlat += rlat_arr[i] * i;
    }
  }
  if (nr_rd)
    avg_rlat /= nr_rd;
  for (uint64_t i = 0; i <= pt_wlat_arr; i++) {
    if (wlat_arr[i]) {
      // ftl_log("write latency: %lu us, count: %lu\n", i, wlat_arr[i]);
      nr_wr += wlat_arr[i];
      avg_wlat += wlat_arr[i] * i;
    }
  }
  if (nr_wr)
    avg_wlat /= nr_wr;
  ftl_log("average read latency: %lu us, average write latency: %lu us\n",
          avg_rlat, avg_wlat);
}

extern uint64_t timer_start_ns;

void init_global_timer() {
  pthread_spin_init(&global_inner_timer_lock, PTHREAD_PROCESS_PRIVATE);
}

void clean_stats() {
  reset_read_worker_state();
  __atomic_store_n(&gc_inflight, 0, __ATOMIC_RELAXED);
  for (uint64_t i = 0; i < MAX_LATENCY_US; i++) {
    wlat_arr[i] = 0;
    rlat_arr[i] = 0;
  }
  ssd_li.stats->nr_nand_read = ssd_li.stats->nr_nand_write =
      ssd_li.stats->nr_nand_erase = 0;
  finished_r = finished_w = 0;
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
  d_cache.stat.hot_promote_blocked_cnt = 0;
  /* hot_valid_entries 是状态计数器，反映 Hot 表驻留数，
   * 不应在不清空 Hot 表时重置，否则与表内容解耦。 */
  // d_cache.stat.hot_valid_entries = 0;
  d_cache.stat.up_grain_cnt = 0;
  d_cache.stat.up_grain_new_cnt = 0;
  d_cache.stat.up_hit_cnt = 0;
  d_cache.stat.up_page_cnt = 0;
  memset(d_cache.stat.grain_heat_distribute, 0, sizeof(uint32_t) * 1000);
  memset(d_cache.stat.hot_pte_state_on_hit, 0,
         sizeof(d_cache.stat.hot_pte_state_on_hit));
#endif
#ifdef ADAPTIVE_MEM
  adaptive_reset_stats(&d_cache);
#endif
  memset(read_latency_samples, 0, sizeof(read_latency_samples));
  read_sample_count = 0;
  fprintf(stdout, "[CLEAN_STATS]\n");
  fflush(stdout);
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

  // 排序延迟样本（升序）
  qsort(samples, sample_count, sizeof(uint64_t), compare_uint64);

  // 计算百分位位置（线性插值，保证精度）
  double index = percentile / 100.0 * (sample_count - 1);
  uint64_t floor_idx = (uint64_t)index;
  uint64_t ceil_idx = floor_idx + 1;
  double frac = index - floor_idx;

  if (ceil_idx >= sample_count) {
    return samples[floor_idx]; // 边界情况：取最后一个值
  }
  // 线性插值计算尾时延
  return (uint64_t)(samples[floor_idx] * (1 - frac) + samples[ceil_idx] * frac);
}

static void show_tail_latency_stats() {
  if (read_sample_count == 0) {
    ftl_log("No read latency samples collected\n");
  } else {
    // 1. 计算平均读延迟
    uint64_t sum_rdlat_us = 0;
    for (uint64_t i = 0; i < read_sample_count; i++) {
      sum_rdlat_us += read_latency_samples[i];
    }
    uint64_t avg_rlat_us = sum_rdlat_us / read_sample_count;

    // 2. 计算尾时延（80/85/90/95/99 分位，注释原 p99.9 可保留或删除）
    uint64_t p95_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 95.0);
    uint64_t p99_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 99.0);
    uint64_t p99_9_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 99.9);
    uint64_t p99_99_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 99.99);

    // 3. 打印尾时延结果（输出 80/85/90/95/99 分位，格式清晰易读）
    ftl_log("========== Tail Latency Stats (iodepth=1) ==========\n");
    ftl_log("avg_rlat/us: %lu | p95_rlat/us: %lu | p99_rlat/us: %lu | "
            "p99.9_rlat/us: %lu | p99.99_rlat/us: %lu\n",
            avg_rlat_us, p95_rlat_us, p99_rlat_us, p99_9_rlat_us,
            p99_99_rlat_us);
  }
  if (write_sample_count == 0) {
    ftl_log("No write latency samples collected\n");
  } else {
    uint64_t sum_wrlat_us = 0;
    for (uint64_t i = 0; i < write_sample_count; i++) {
      sum_wrlat_us += write_latency_samples[i];
    }
    uint64_t avg_wlat_us = sum_wrlat_us / write_sample_count;
    uint64_t p95_wlat_us =
        calculate_tail_latency(write_latency_samples, write_sample_count, 95.0);
    uint64_t p99_wlat_us =
        calculate_tail_latency(write_latency_samples, write_sample_count, 99.0);
    uint64_t p99_9_wlat_us =
        calculate_tail_latency(write_latency_samples, write_sample_count, 99.9);
    ftl_log("========== Tail Latency Stats (iodepth=1) ==========\n");
    ftl_log("write samples count: %lu\n", write_sample_count);
    ftl_log("avg_wlat/us: %lu | p95_wlat/us: %lu | p99_wlat/us: %lu | "
            "p99.9_wlat/us: %lu\n",
            avg_wlat_us, p95_wlat_us, p99_wlat_us, p99_9_wlat_us);
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
  show_latency_stats();
  ftl_log("finished_r: %lu, finished_w: %lu, wrong_value_cnt: %lu\n",
          finished_r, finished_w, wrong_value_cnt);
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
  ftl_log("read iops: %0.2f,  write iops: %0.2f, update iops: %0.2f, hit "
          "rt:%0.2f%%\n",
          read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
          update_iops / update_batch_cnt,
          (double)(d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit) /
              (d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit +
               d_cache.stat.cache_miss) *
              100);
  /* hot_valid_entries：当前 Hot 表驻留 grain 数（快照，非累计） */
  ftl_log("hot valid (current): entries=%lu pages=%lu / max_hot_pages=%lu\n",
          d_cache.stat.hot_valid_entries, d_cache.stat.hot_valid_entries / EPP,
          d_cache.env.max_cached_hot_tpages);
  double avg_grain_per_page =
      d_cache.stat.up_page_cnt
          ? (double)d_cache.stat.up_grain_cnt / (double)d_cache.stat.up_page_cnt
          : 0.0;
  double avg_grain_hit =
      d_cache.stat.up_grain_cnt
          ? (double)d_cache.stat.up_hit_cnt / (double)d_cache.stat.up_grain_cnt
          : 0.0;
  ftl_log("avg_grain_per_page: %.2f, avg_grain_hit: %.2f\n", avg_grain_per_page,
          avg_grain_hit);
  ftl_log("promote (lifetime): cold_pages=%lu grain_attempts=%lu "
          "grain_new_insert=%lu "
          "equiv_pages_attempts=%lu equiv_pages_new=%lu churn=%.2fx\n",
          d_cache.stat.up_page_cnt, d_cache.stat.up_grain_cnt,
          d_cache.stat.up_grain_new_cnt, d_cache.stat.up_grain_cnt / EPP,
          d_cache.stat.up_grain_new_cnt / EPP,
          d_cache.stat.hot_valid_entries
              ? (double)d_cache.stat.up_grain_new_cnt /
                    (double)d_cache.stat.hot_valid_entries
              : 0.0);
  ftl_log("hot_hit: %lu, hot_rewrite_entries: %lu, hot_promote_blocked: %lu\n",
          d_cache.stat.hot_cmt_hit, d_cache.stat.hot_rewrite_entries,
          d_cache.stat.hot_promote_blocked_cnt);
  {
    uint64_t hot_state_hit_sum = 0;
    for (int i = 0; i < STATE_NUM; i++)
      hot_state_hit_sum += d_cache.stat.hot_pte_state_on_hit[i];
    if (hot_state_hit_sum > 0) {
      ftl_log("hot_pte_state_on_hit (after raise, %% of hot_hit):\n");
      for (int i = 0; i < STATE_NUM; i++) {
        if (d_cache.stat.hot_pte_state_on_hit[i] > 0)
          ftl_log("  state[%d]: %0.6f%%\n", i,
                  (double)d_cache.stat.hot_pte_state_on_hit[i] /
                      (double)d_cache.stat.hot_cmt_hit * 100.0);
      }
    }
  }
  for (int i = 0; i < STATE_NUM; i++) {
    if (d_cache.stat.grain_heat_distribute[i] > 0)
      ftl_log("cold_cnt_at_promote[%d]: %0.6f%%\n", i,
              (double)d_cache.stat.grain_heat_distribute[i] /
                  d_cache.stat.up_page_cnt / EPP * 100);
  }
#else
  ftl_log(
      "read iops: %0.2f,write iops: %0.2f,update iops: %0.2f,hit rt:%0.2f%%\n",
      read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
      update_iops / update_batch_cnt,
      (d_cache.stat.cache_hit) /
          (double)(d_cache.stat.cache_hit + d_cache.stat.cache_miss) * 100);
#endif
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
  ftl_log("total_page_heat_sum: %" PRIu64 ", total_inpage: %" PRIu64
          ", avg_page_heat: %.4f\n",
          total_hit_inpage, total_inpage, avg_hit_inpage);
  show_tail_latency_stats();
#ifdef ADAPTIVE_MEM
  adaptive_show_stats(&d_cache);
#endif
  fprintf(stdout, "[END_STATS]\n");
  fflush(stdout);
}

static void log_update_gc_stats(const algorithm *palgo, uint64_t num_update) {
  uint64_t num_data = D_ENV(palgo)->num_data_gc;
  uint64_t num_reads = D_ENV(palgo)->num_gc_flash_read;
  uint64_t num_gc_mapping_hit = D_ENV(palgo)->num_gc_mapping_hit;
  uint64_t num_gc_cmt = D_ENV(palgo)->gc_cmt_total;

  ftl_log(
      "gc data: %lu, gc mapping read: %lu, avg read: %.2f, gc mapping hit: "
      "%lu, gc mapping hit rt: %.4f, gc cmt total: %lu, avg cmt per gc: %.2f\n",
      num_data, num_reads, num_data ? (float)num_reads / num_data : 0.f,
      num_gc_mapping_hit,
      (num_reads + num_gc_mapping_hit)
          ? (float)num_gc_mapping_hit / (num_reads + num_gc_mapping_hit)
          : 0.f,
      num_gc_cmt, num_data ? (float)num_gc_cmt / num_data : 0.f);
  ftl_log("read for data check: %u, avg update data check: %0.2f\n",
          D_ENV(palgo)->num_rd_data_rd,
          num_update ? (float)D_ENV(palgo)->num_rd_data_rd / num_update : 0.f);
}

/* 输出 read_bench 的单行汇总结果：stdout + 追加写入 ./hash_kvssd_results/read_test_summary.csv */
static void emit_read_bench_summary(uint64_t pool_size, float map_size_frac,
                                    const char *read_dist, uint64_t read_num,
                                    int iodepth_used, double read_iops_val,
                                    double hit_rt_pct) {
  const char *csv_path = "./hash_kvssd_results/read_test_summary.csv";
  FILE *fp = fopen(csv_path, "a");
  if (fp == NULL) {
    ftl_warn("failed to open %s for append, summary will only print to stdout\n",
             csv_path);
  } else {
    /* 文件为空时写入表头 */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz == 0) {
      fprintf(fp, "pool_size,map_size_frac,read_dist,read_num,iodepth,"
                  "read_iops,hit_rt_pct\n");
    }
    fprintf(fp, "%lu,%.4f,%s,%lu,%d,%.2f,%.4f\n", pool_size,
            (double)map_size_frac, read_dist, read_num, iodepth_used,
            read_iops_val, hit_rt_pct);
    fclose(fp);
  }
  ftl_log("[SUMMARY] pool_size=%lu, map_size_frac=%.4f, read_dist=%s, "
          "read_num=%lu, iodepth=%d, read_iops=%.2f, hit_rt=%.4f%%\n",
          pool_size, (double)map_size_frac, read_dist, read_num, iodepth_used,
          read_iops_val, hit_rt_pct);
}

/* 输出 update_bench 的单行汇总结果：stdout + 追加写入 ./hash_kvssd_results/update_test_summary.csv */
static void emit_update_bench_summary(uint64_t pool_size, float map_size_frac,
                                       const char *update_dist,
                                       uint64_t update_num, int iodepth_used,
                                       double update_iops_val,
                                       double hit_rt_pct) {
  const char *csv_path = "./hash_kvssd_results/update_test_summary.csv";
  FILE *fp = fopen(csv_path, "a");
  if (fp == NULL) {
    ftl_warn("failed to open %s for append, summary will only print to stdout\n",
             csv_path);
  } else {
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz == 0) {
      fprintf(fp, "pool_size,map_size_frac,update_dist,update_num,iodepth,"
                  "update_iops,hit_rt_pct\n");
    }
    fprintf(fp, "%lu,%.4f,%s,%lu,%d,%.2f,%.4f\n", pool_size,
            (double)map_size_frac, update_dist, update_num, iodepth_used,
            update_iops_val, hit_rt_pct);
    fclose(fp);
  }
  ftl_log("[SUMMARY] pool_size=%lu, map_size_frac=%.4f, update_dist=%s, "
          "update_num=%lu, iodepth=%d, update_iops=%.2f, hit_rt=%.4f%%\n",
          pool_size, (double)map_size_frac, update_dist, update_num, iodepth_used,
          update_iops_val, hit_rt_pct);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  ycsb_latency_samples = calloc(MAX_YCSB_SAMPLES, sizeof(uint64_t));
  read_latency_samples = calloc(MAX_READ_SAMPLES, sizeof(uint64_t));
  write_latency_samples = calloc(MAX_WRITE_SAMPLES, sizeof(uint64_t));
  gc_latency_samples = calloc(MAX_GC_SAMPLES, sizeof(uint64_t));
  struct option opts[] = {
      {"pool_size", required_argument, NULL, 0},
      {"num_update", required_argument, NULL, 1},
      {"num_read", required_argument, NULL, 2},
      {"map_size_frac", required_argument, NULL, 3},
      {"seed", required_argument, NULL, 4},
      {"ext_mem_lat", required_argument, NULL, 5},
      {"zipf_alpha", required_argument, NULL, 6},
      {"keylen_mean", required_argument, NULL, 7},
      {"variable_keylen", no_argument, NULL, 8},
      {"ycsb", required_argument, NULL, 9},
      {"ycsb_ops", required_argument, NULL, 10},
      {"read_dist", required_argument, NULL, 11},
      {"read_bench", no_argument, NULL, 12},
      {"update_dist", required_argument, NULL, 13},
      {"update_bench", no_argument, NULL, 14},
      {"ssd_lat_off", no_argument, NULL, 15},
      {"experiment_tag", required_argument, NULL, 16},
      {"quiet", no_argument, NULL, 17},
      {0, 0, 0, 0},
  };
  // for read test
  ftl_log("hello world\n");
  uint64_t nr_G_workload = 1048576;
  int64_t pool_size = 64 * nr_G_workload;
  uint64_t num_update = 8 * nr_G_workload;
  uint64_t num_read = 64 * nr_G_workload / NUM_WORKERS;
  float map_size_frac = 256.0 / 256;

  int seed = 1;
  uint64_t ext_mem_lat = 0;
  char *shortopts = "";

  // YCSB相关参数
  bool run_ycsb = false;
  char ycsb_workload = 'a';
  uint64_t ycsb_num_ops = 44000000;

  // 单轮读测试模式：与 YCSB 互斥，仅跑 load + 一次 test_read
  bool run_read_bench = false;
  // 单轮更新测试模式：与 YCSB/read_bench 互斥，仅跑 load + 一次 test_update
  bool run_update_bench = false;
  // 关闭 SSD 延迟标志（加速 GC 磨损测试）
  bool ssd_lat_off = false;
  // 实验标签（脚本解析用）：可选项，打印 [EXP_TAG] 行到日志
  const char *experiment_tag = NULL;
  // 静默模式：抑制常见 INFO/WARMUP/finish 日志，仅保留 STAT/YCSB/SUMMARY/EXP_TAG/END_STATS
  bool quiet_mode = false;

  int ret = 0;
  int option_index = 0;
  while ((ret = getopt_long(argc, argv, shortopts, opts, &option_index)) !=
         -1) {
    switch (ret) {
    case 0:
      // pool_size
      pool_size = atoi(optarg);
      break;
    case 1:
      // num_update
      num_update = atoi(optarg);
      break;
    case 2:
      // num_read
      num_read = atoi(optarg);
      break;
    case 3:
      // mapping_size_frac
      // 必须是 (0, 1] 之间的浮点：原 main 中只有 map_size_frac < 1 才会缩容
      // 传整数（>=1）会被忽略，与"map_sizefrac 不会大于 1"约束一致。
      map_size_frac = (float)atof(optarg);
      if (map_size_frac <= 0.0f || map_size_frac > 1.0f) {
        ftl_err("map_size_frac must be in (0, 1], got %s\n", optarg);
        abort();
      }
      break;
    case 4:
      // seed
      seed = atoi(optarg);
      break;
    case 5:
      // ext_mem_lat
      ext_mem_lat = atoi(optarg);
      break;
    case 6:
      // zipf_alpha
      READ_ALPHA = atof(optarg);
      UPDATE_ALPHA = READ_ALPHA;
      break;
    case 7:
      // keylen_mean (16/32/64B三档)
      KEYLEN_MEAN = atoi(optarg);
      break;
    case 8:
      // variable_keylen
      ENABLE_VARIABLE_KEYLEN = true;
      break;
    case 9:
      // ycsb
      run_ycsb = true;
      ycsb_workload = *optarg;
      break;
    case 10:
      // ycsb_ops
      ycsb_num_ops = atoll(optarg);
      break;
    case 11:
      // read_dist
      if (strcmp(optarg, "uniform") == 0)
        read_dist_mode = 1;
      else if (strcmp(optarg, "zipfian") == 0)
        read_dist_mode = 0;
      else
        ftl_err("Unknown read_dist: %s (use uniform or zipfian)\n", optarg);
      break;
    case 12:
      // read_bench: 仅做 load + 一轮 test_read
      run_read_bench = true;
      break;
    case 13:
      // update_dist
      if (strcmp(optarg, "uniform") == 0)
        update_dist_mode = 1;
      else if (strcmp(optarg, "zipfian") == 0)
        update_dist_mode = 0;
      else
        ftl_err("Unknown update_dist: %s (use uniform or zipfian)\n", optarg);
      break;
    case 14:
      // update_bench: 仅做 load + 一轮 test_update
      run_update_bench = true;
      break;
    case 15:
      // ssd_lat_off: 关闭 SSD 延迟（加速 GC 磨损测试）
      ssd_lat_off = true;
      break;
    case 16:
      // experiment_tag: 自定义实验标签，写入日志便于脚本解析
      experiment_tag = optarg;
      break;
    case 17:
      // quiet: 抑制 INFO/WARMUP 等非关键日志
      quiet_mode = true;
      break;
    case '?':
      break;
    }
  }
  if (!pool_size) {
    ftl_err("need pool_size\n");
    abort();
  }
  /* 实验标签标记：脚本解析用 */
  if (experiment_tag) {
    fprintf(stdout, "[EXP_TAG] %s\n", experiment_tag);
    fflush(stdout);
  }
  /* 静默模式下抑制非关键日志：通过环境变量 Q=1 让 ftl_log 早返 */
  if (quiet_mode) {
    setenv("QUIET_MODE", "1", 1);
  }
  ftl_log("pool_size = %lu, num_update = %lu, num_read = %lu, map_size_frac = "
          "%0.2f, seed = %d, ext_mem_lat = %lu\n"
          "zipf_alpha = %.2f, keylen_mean = %dB (std %.1fB), variable_keylen = "
          "%s\n",
          pool_size, num_update, num_read, map_size_frac, seed, ext_mem_lat,
          READ_ALPHA, KEYLEN_MEAN, KEYLEN_MEAN * KEYLEN_STD_PERCENT,
          ENABLE_VARIABLE_KEYLEN ? "yes" : "no");
  // env create
  extra_mem_lat = ext_mem_lat;
  timer_start_ns = clock_get_ns();
  algorithm *palgo = &__demand;
  ssd_li.create(&ssd_li);
  palgo->create(palgo, &ssd_li);
  init_global_timer();
  if (map_size_frac < 1) {
    d_cache.env.nr_valid_tpages *= map_size_frac;
    d_cache.env.nr_valid_tentries = d_cache.env.nr_valid_tpages * EPP;
  }
#ifdef HOT_CMT
  {
    uint64_t cold_mb =
        (uint64_t)d_cache.env.max_cached_tpages * PAGESIZE / 1024 / 1024;
    uint64_t hot_mb =
        (uint64_t)d_cache.env.max_cached_hot_tpages * PAGESIZE / 1024 / 1024;
#ifdef VERIFY_CACHE
    uint64_t key_mb = (uint64_t)(d_cache.env.max_cached_full_key_budget /
                                 1024 / 1024);

    ftl_log("hash_table_size: %lu MB, cached: %lu MB (cold %lu + hot %lu + key "
            "%lu MiB)\n",
            (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024,
            cold_mb + hot_mb + key_mb, cold_mb, hot_mb, key_mb);
#else
    ftl_log("hash_table_size: %lu MB, cached: %lu MB (cold %lu + hot %lu MiB; "
            "key budget returned to pool)\n",
            (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024,
            cold_mb + hot_mb, cold_mb, hot_mb);
#endif
  }
#else
  ftl_log("hash_table_size: %lu MB, cached: %lu MB\n",
          (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024,
          (uint64_t)d_cache.env.max_cached_tpages * PAGESIZE / 1024 / 1024);
#endif
#ifdef ADAPTIVE_MEM
  adaptive_show_stats_event(&d_cache, ADAPTIVE_EV_INIT,
                            "initial partition after env setup");
#endif
  fflush(stdout);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&algo_tr, &attr, algo_thread, NULL);
  pthread_attr_t cq_cpl_attr;
  pthread_attr_init(&cq_cpl_attr);
  pthread_create(&finish_tr[0], &cq_cpl_attr, process_cq_cpl, NULL);
  pthread_create(&finish_tr[1], &cq_cpl_attr, process_cq_cpl, NULL);
  shuffle_map = create_shuffle_map(pool_size - 1);
  // test
  ftl_log("start test.\n");
  /* data load — SSD latency off for fast fill; load up to CMT capacity */
  iodepth = 64;
  ftl_log("start loading. iodepth: %d, ssd_lat=off\n", iodepth);
  toggle_ssd_lat(false);
  uint64_t load_entries = pool_size;
  if (load_entries > d_cache.env.nr_valid_tentries)
      load_entries = d_cache.env.nr_valid_tentries;
  ftl_log("loading %lu entries (pool_size=%lu, CMT_capacity=%lu)\n",
          load_entries, pool_size, d_cache.env.nr_valid_tentries);
  test_load(palgo, (int)load_entries);
  sleep(5);
  clean_stats();
  /* Drop load-phase Hot CMT pollution before benchmark */
#ifdef HOT_CMT
  d_cache.hot_cmt_reset(&d_cache);
  ftl_log("Hot CMT reset after load (hot_valid=0, ready for benchmark)\n");
#ifdef ADAPTIVE_MEM
  adaptive_prepare_for_benchmark(&d_cache);
  adaptive_show_stats_event(&d_cache, ADAPTIVE_EV_INIT,
                            "post-load reset before benchmark");
#endif
#endif
  /* warm-up: zipfian read, SSD latency off */
  // {
  //   uint64_t warmup_ops_per_worker = 128 * nr_G_workload / NUM_WORKERS;
  //   uint64_t warmup_update_per_worker = 128 * nr_G_workload / NUM_WORKERS;
  //   iodepth = 32;
  //   // 先读预热
  //   ftl_log("start zipfian warmup read. ...");
  //   test_read(palgo, pool_size, warmup_ops_per_worker, true, false, seed,
  //             NUM_WORKERS);
  //   ftl_log("finish zipfian warmup read.\n");
  //   sleep(2);
  //   // clean_stats();

  //   // 再更新预热：触发 Key Cache 填充与扩容决策
  //   ftl_log("start zipfian warmup update. ...");
  //   test_update(palgo, pool_size / 2, warmup_update_per_worker, true, seed,
  //               NUM_WORKERS);
  //   ftl_log("finish zipfian warmup update.\n");
  //   fflush(stdout);
  //   sleep(5);
  //   clean_stats();
  // }

  if (run_ycsb) {
    // 运行YCSB负载（iodepth 保持 warmup 设置的 32）
    ftl_log("[YCSB] iodepth: %d for benchmark\n", iodepth);
    toggle_ssd_lat(true);
    ftl_log("SSD latency turned on for benchmark\n");
    ftl_log("========== Start YCSB Workload %c ==========\n", ycsb_workload);
    ftl_log("[YCSB CONFIG] VariableKeyLen=%s, KeyLenMean=%dB (std=%.1fB), "
            "ZipfAlpha=%.2f, NumOps=%lu, Workers=%d\n",
            ENABLE_VARIABLE_KEYLEN ? "yes" : "no", KEYLEN_MEAN,
            KEYLEN_MEAN * KEYLEN_STD_PERCENT, READ_ALPHA, ycsb_num_ops,
            NUM_WORKERS);
    ycsb_run_workload(ycsb_workload, palgo, pool_size, ycsb_num_ops,
                      NUM_WORKERS, seed);
  } else if (run_read_bench) {
    /* 单轮纯读测试模式：与 YCSB 互斥
     * - 不做 warmup
     * - iodepth=32（与原 zipfian 读测试保持一致）
     * - read_num 默认 = pool_size（与用户“read num 等于 pool size”语义一致）
     * - 结束后输出 [SUMMARY] 并退出 */
    if (num_read == 0) {
      num_read = pool_size / NUM_WORKERS;
    }
    iodepth = 32;
    bool is_zipf = (read_dist_mode == 0);
    ftl_log("========== Start Read Bench ==========\n");
    ftl_log("[READ_BENCH CONFIG] pool_size=%lu, map_size_frac=%.4f, "
            "read_dist=%s, read_num=%lu, iodepth=%d, num_workers=%d\n",
            pool_size, (double)map_size_frac, is_zipf ? "zipfian" : "uniform",
            num_read, iodepth, NUM_WORKERS);
    toggle_ssd_lat(true);
    test_read(palgo, pool_size, num_read, is_zipf, /*is_seq=*/false, seed,
              NUM_WORKERS);
    fflush(stdout);
    sleep(2);
    show_tail_latency_stats();
    /* 计算 read_iops（与 show_stats 保持同样口径：read_iops/(read_batch_cnt-1)） */
    double iops_val = (read_batch_cnt > 1)
                          ? (read_iops / (double)(read_batch_cnt - 1))
                          : 0.0;
    uint64_t hit = d_cache.stat.cache_hit;
    uint64_t miss = d_cache.stat.cache_miss;
    double hit_rt = (hit + miss) ? (double)hit / (double)(hit + miss) * 100.0
                                 : 0.0;
    emit_read_bench_summary(pool_size, map_size_frac,
                            is_zipf ? "zipfian" : "uniform", num_read, iodepth,
                            iops_val, hit_rt);
    ftl_log("========== Finish Read Bench ==========\n");
  } else if (run_update_bench) {
    /* 单轮纯更新测试模式：与 YCSB/read_bench 互斥
     * - 不做 warmup
     * - iodepth=32
     * - update_num 默认 = pool_size / NUM_WORKERS（per-worker，总量 = pool_size）
     * - 结束后输出 [SUMMARY] 并退出 */
    if (num_update == 0) {
      num_update = pool_size / NUM_WORKERS;
    }
    iodepth = 32;
    bool is_zipf = (update_dist_mode == 0);
    ftl_log("========== Start Update Bench ==========\n");
    ftl_log("[UPDATE_BENCH CONFIG] pool_size=%lu, map_size_frac=%.4f, "
            "update_dist=%s, update_num=%lu, iodepth=%d, num_workers=%d\n",
            pool_size, (double)map_size_frac, is_zipf ? "zipfian" : "uniform",
            num_update, iodepth, NUM_WORKERS);
    toggle_ssd_lat(ssd_lat_off ? false : true);
    test_update(palgo, pool_size, num_update, is_zipf, seed, NUM_WORKERS);
    fflush(stdout);
    sleep(2);
    /* 计算 update_iops：update_iops / update_batch_cnt（与 show_stats 口径一致） */
    double iops_val =
        update_batch_cnt > 0 ? update_iops / (double)update_batch_cnt : 0.0;
    uint64_t hit = d_cache.stat.cache_hit;
    uint64_t miss = d_cache.stat.cache_miss;
    double hit_rt = (hit + miss) ? (double)hit / (double)(hit + miss) * 100.0
                                 : 0.0;
    emit_update_bench_summary(pool_size, map_size_frac,
                              is_zipf ? "zipfian" : "uniform", num_update,
                              iodepth, iops_val, hit_rt);
    bm_show_gc_cnt_stats(pbm);
    ftl_log("========== Finish Update Bench ==========\n");
  } else {
    // 原来的测试逻辑
    // iodepth = 32;
    // test_update(palgo, pool_size, num_update, true, seed, 1);
    // log_update_gc_stats(palgo, num_update);
    // ftl_log("update finished.\n");
    // fflush(stdout);
    // sleep(2);
    // clean_stats();

    // /* seq read */
    // iodepth = 32;
    // ftl_log("start seq reading. iodepth: %d\n", iodepth);
    // toggle_ssd_lat(true);
    // test_read(palgo, pool_size, num_read * NUM_WORKERS, false, true, seed,
    // 1); ftl_log("finish seq reading.\n"); fflush(stdout); sleep(2);
    // clean_stats();

    // /* random read */
    // iodepth = 1;
    // ftl_log("start random reading. iodepth: %d\n", iodepth);
    // toggle_ssd_lat(true);
    // test_read(palgo, pool_size, num_read, false, false, seed, NUM_WORKERS);
    // ftl_log("finish random reading.\n");
    // fflush(stdout);
    // sleep(2);
    // clean_stats();

    /* zipfian read */
    iodepth = 32;
    ftl_log("start zipfian reading. iodepth: %d\n", iodepth);
    toggle_ssd_lat(true);
    test_read(palgo, pool_size, num_read, true, false, seed, NUM_WORKERS);
    ftl_log("finish zipfian reading.\n");
    fflush(stdout);
    sleep(2);
    show_stats();
  }
  return 0;
}

//---------------ycsb test functions---------------------//

// YCSB E workload - 扫描线程实现
void *test_rmw_tr(void *args) {
  struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } *pargs = args;
  algorithm *palgo = pargs->palgo;
  uint64_t max = pargs->max;
  uint64_t num = pargs->num;
  bool is_zipf = pargs->is_zipf;
  int seed = pargs->seed;
  volatile int tr_nr_ios = 0;

  struct zipf_state zs;
  unsigned int local_seed = seed + pthread_self();
  srand(local_seed);
  zipf_init(&zs, max - 1, UPDATE_ALPHA, -1, seed);
  if (shuffle_map)
    zipf_use_shuffle_map(&zs, shuffle_map);

  while (!(*pargs->pstart))
    ;

  for (uint64_t i = 0; i < num; ++i) {
    uint64_t rndkey;
    value_set *r_value = NULL;
    request *r_req = NULL;
    value_set *w_value = NULL;
    request *w_req = NULL;

    if (is_zipf)
      rndkey = zipf_next(&zs) + 1;
    else
      rndkey = rand_r(&local_seed) % (max - 1) + 1;

    if (rndkey > max)
      rndkey = max;

    r_value = inf_get_valueset(NULL, PAGESIZE); // 接收读返回值
    r_req = g_malloc0(sizeof(request));
    r_req->type = DATAR;
    fill_request_key(&r_req->key, rndkey);
    r_req->h_params = NULL;
    r_req->params = NULL;
    r_req->state = ALGO_REQ_PENDING;
    r_req->value = r_value;
    r_req->end_req = end_request;
    r_req->ptr_nr_ios = &tr_nr_ios;
    tr_nr_ios = 0;
    submit_req(palgo, r_req);

    while (tr_nr_ios > 0)
      ;
    usleep(10);

    char modified_value[8];
    for (int j = 0; j < 7; j++) {
      modified_value[j] = '0' + (int)(rand_r(&local_seed) % 10);
    }
    modified_value[7] = '\0';
    w_value = inf_get_valueset(modified_value, 7);

    w_req = g_malloc0(sizeof(request));
    w_req->type = DATAW;
    fill_request_key(&w_req->key, rndkey);
    w_req->h_params = NULL;
    w_req->params = NULL;
    w_req->value = w_value;
    w_req->state = ALGO_REQ_PENDING;
    w_req->end_req = end_request;
    w_req->ptr_nr_ios = &tr_nr_ios;
    tr_nr_ios = 0;
    submit_req(palgo, w_req);

    // 等待写请求完成（保证本次RMW原子性完成）
    while (tr_nr_ios > 0)
      ;
    usleep(10);
  }

  usleep(2000000); // 等待最后一批请求完成，合理保留
  return NULL;
}

static void *test_scan_tr(void *args) {
  static int worker_cnt = 0;
  int worker_id = g_atomic_int_add(&worker_cnt, 1);
  prctl(PR_SET_NAME, "scan_worker");

  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    uint64_t scan_len;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } ScanArgs;
  ScanArgs *pargs = (ScanArgs *)args;

  algorithm *palgo = pargs->palgo;
  uint64_t max = pargs->max;
  uint64_t num = pargs->num;
  uint64_t scan_len = pargs->scan_len;
  bool is_zipf = pargs->is_zipf;
  uint32_t seed = pargs->seed + worker_id;
  volatile int tr_nr_ios = 0;
  struct zipf_state zs;

  if (is_zipf) {
    zipf_init(&zs, max - 1, READ_ALPHA, -1, seed);
    if (shuffle_map != NULL)
      zipf_use_shuffle_map(&zs, shuffle_map);
  }
  srand(seed);

  while (!(*pargs->pstart))
    ;

  for (uint64_t i = 0; i < num; ++i) {
    uint64_t start_key;
    if (is_zipf) {
      start_key = zipf_next(&zs) + 1;
    } else {
      start_key = rand_r(&seed) % (max - scan_len) + 1;
    }

    for (uint64_t s = 0; s < scan_len; ++s) {
      uint64_t cur_key = start_key + s;
      if (cur_key > max)
        break;

      value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
      request *r_req = g_malloc0(sizeof(request));
      r_req->type = DATAR;
      r_req->key.len = sprintf(r_req->key.key, "%lu", cur_key);
      r_req->h_params = NULL;
      r_req->params = NULL;
      r_req->state = ALGO_REQ_PENDING;
      r_req->value = r_value;
      r_req->end_req = end_request;
      r_req->ptr_nr_ios = &tr_nr_ios;
      submit_req(palgo, r_req);
    }
  }

  if (worker_id >= 0 && worker_id < NUM_WORKERS)
    thread_ended[worker_id] = true;
  usleep(2000000);
  return NULL;
}

void ycsb_run_workload(char workload, algorithm *palgo, uint64_t pool_size,
                       uint64_t num_ops, int nr_workers, int seed) {
  ftl_log("[YCSB CONFIG] iodepth: %d\n", iodepth);
  ycsb_running = true;
  ftl_log("========== Start YCSB Workload %c ==========\n", workload);
  uint64_t ycsb_start_ns = clock_get_ns();
  double ycsb_read_iops = 0, ycsb_write_iops = 0;
  uint64_t ycsb_read_cnt = 0, ycsb_write_cnt = 0;

  // 重置统计量
  uint64_t prev_finished_r = finished_r;
  uint64_t prev_finished_w = finished_w;
  clean_stats();
  // 调用对应负载的独立函数
  switch (tolower(workload)) {
  case 'a':
    test_ycsb_a(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'b':
    test_ycsb_b(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'c':
    test_ycsb_c(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'd':
    test_ycsb_d(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'e':
    test_ycsb_e(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'f':
    test_ycsb_f(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  default:
    ftl_err("Unsupported YCSB workload: %c (only a-f are supported)\n",
            workload);
    abort();
  }

  ycsb_read_cnt = finished_r - prev_finished_r;
  ycsb_write_cnt = finished_w - prev_finished_w;

  uint64_t final_ycsb_total_ops = g_atomic_pointer_get(&ycsb_total_ops);
  uint64_t final_ycsb_total_lat_ns = g_atomic_pointer_get(&ycsb_total_lat_ns);
  uint64_t final_ycsb_max_lat_us = g_atomic_pointer_get(&ycsb_max_lat_us);
  double final_ycsb_avg_iops =
      ycsb_batch_cnt > 0 ? (ycsb_total_iops / ycsb_batch_cnt) : 0.0;
  uint64_t final_ycsb_avg_lat_us = 0;
  if (ycsb_batch_cnt > 0) {
    final_ycsb_avg_lat_us = ycsb_total_batch_avg_lat_us / ycsb_batch_cnt;
  }
  ftl_log("[YCSB GLOBAL] Total ops: %lu, Avg IOPS: %.2f, Avg latency: %lu us, "
          "Max latency: %lu us\n",
          final_ycsb_total_ops, final_ycsb_avg_iops, final_ycsb_avg_lat_us,
          final_ycsb_max_lat_us);
  ftl_log("[YCSB CONFIG] Workload=%c, VariableKeyLen=%s, KeyLenMean=%dB "
          "(std=%.1fB), ZipfAlpha=%.2f\n",
          workload, ENABLE_VARIABLE_KEYLEN ? "yes" : "no", KEYLEN_MEAN,
          KEYLEN_MEAN * KEYLEN_STD_PERCENT, READ_ALPHA);
  ftl_log("[YCSB HIT RATE] Cache Hit: %.2f%% (hit: %lu, miss: %lu)\n",
          d_cache.stat.cache_hit * 100.0 /
              (d_cache.stat.cache_hit + d_cache.stat.cache_miss),
          d_cache.stat.cache_hit, d_cache.stat.cache_miss);
  show_stats();
  ftl_log("========== Finish YCSB Workload %c ==========\n", workload);
  ycsb_running = false;
}

void test_ycsb_a(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  uint64_t num_update_total = num_ops * 0.5;
  uint64_t num_read_per_worker = (num_ops * 0.5) / nr_workers;

  // 并行执行：1个update线程 + nr_workers个read线程
  volatile bool start = false;
  pthread_t update_worker;
  pthread_t read_workers[nr_workers];
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns;
  rd_start_ns = parallel_start_ns;

  // 构造update线程参数
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } UpdateArgs;
  UpdateArgs update_args = {.palgo = palgo,
                            .max = pool_size / 2,
                            .num = num_update_total,
                            .is_zipf = true,
                            .seed = seed,
                            .pstart = &start};

  // 构造read线程参数
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size / 2,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // 创建线程（等待启动状态）
  ftl_log(
      "YCSB Workload a: Create 1 update thread + %d read threads (parallel)\n",
      nr_workers);
  pthread_create(&update_worker, NULL, test_update_tr, &update_args);
  for (int i = 0; i < nr_workers; ++i) {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // 等待线程就绪，统一启动
  sleep(5);
  ftl_log("YCSB Workload a: Start update + read (parallel)\n");
  start = true;

  // 等待所有线程结束
  pthread_join(update_worker, NULL);
  for (int i = 0; i < nr_workers; ++i) {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload a: Parallel execution finished\n");
}

void test_ycsb_b(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  uint64_t num_update_total = num_ops * 0.05;
  uint64_t num_read_per_worker = (num_ops * 0.95) / nr_workers;

  // 并行执行：1个update线程 + nr_workers个read线程
  volatile bool start = false;
  pthread_t update_worker;
  pthread_t read_workers[nr_workers];
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns;
  rd_start_ns = parallel_start_ns;

  // 构造update线程参数
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } UpdateArgs;
  UpdateArgs update_args = {.palgo = palgo,
                            .max = pool_size,
                            .num = num_update_total,
                            .is_zipf = true,
                            .seed = seed,
                            .pstart = &start};

  // 构造read线程参数
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // 创建线程（等待启动状态）
  ftl_log(
      "YCSB Workload b: Create 1 update thread + %d read threads (parallel)\n",
      nr_workers);
  pthread_create(&update_worker, NULL, test_update_tr, &update_args);
  for (int i = 0; i < nr_workers; ++i) {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // 等待线程就绪，统一启动
  sleep(5);
  ftl_log("YCSB Workload b: Start update + read (parallel)\n");
  start = true;

  // 等待所有线程结束
  pthread_join(update_worker, NULL);
  for (int i = 0; i < nr_workers; ++i) {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload b: Parallel execution finished\n");
}

void test_ycsb_c(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  /*
   * 与 B 对齐并发：B 用 1 update + 6 read = 7 线程；
   * C 纯读少了一个 update 线程，设为 7 读线程弥补。
   * iodepth=64 下每线程可 pipeline 64 请求，7 vs 6 线程约 +16.7% 并发；
   * 余下 gap（~4-5%）来自 B 的 update 线程对 mapping LRU 的保活效应，
   * 纯读负载无法消除。
   */
  int n_readers = nr_workers + 1;
  uint64_t num_read_per_worker = num_ops / n_readers;
  ftl_log("YCSB Workload c: Start read phase (%d read threads, target "
          "concurrency=%d)\n",
          n_readers, n_readers);
  test_read(palgo, pool_size, num_read_per_worker, read_dist_mode == 0, false,
            seed, n_readers);
  ftl_log("YCSB Workload c: Read phase finished\n");
}

void test_ycsb_d(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  uint64_t num_insert_total = num_ops * 0.05; // 5%插入（调用原test_load）
  uint64_t num_read_per_worker =
      (num_ops * 0.95) / nr_workers; // 95%读（多线程）

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t insert_worker;            // 1个插入线程（包装test_load）
  pthread_t read_workers[nr_workers]; // nr_workers个读线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // 插入（写）时间基准对齐
  rd_start_ns = parallel_start_ns; // 读时间基准对齐

  // ========== 构造插入线程参数（传给包装函数） ==========
  typedef struct {
    algorithm *palgo;
    uint64_t base; // 起始 key
    uint64_t num;
    volatile bool *pstart;
  } InsertArgs;
  InsertArgs insert_args = {.palgo = palgo,
                            .base = pool_size + 1,
                            .num = num_insert_total,
                            .pstart = &start};

  // ========== 构造读线程参数 ==========
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = (int)(pool_size + num_insert_total + 1),
                        .num = num_read_per_worker,
                        .is_zipf = false, // D负载读不使用zipf
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload d: Create 1 insert thread + %d read "
          "threads (parallel)\n",
          nr_workers);
  // 创建插入线程（写入新 key，不覆盖已加载数据）
  pthread_create(&insert_worker, NULL, test_insert_latest_tr, &insert_args);
  // 创建读线程
  for (int i = 0; i < nr_workers; ++i) {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5); // 等待所有线程初始化完成
  ftl_log("YCSB Workload d: Start insert (test_load) + read (parallel)\n");
  start = true;

  // ========== 等待所有线程执行完毕 ==========
  // 等待插入线程结束（test_load执行完成）
  pthread_join(insert_worker, NULL);
  ftl_log("YCSB Workload d: Insert thread (test_load) finished\n");
  // 等待所有读线程结束
  for (int i = 0; i < nr_workers; ++i) {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload d: All read threads finished\n");
  ftl_log("YCSB Workload d: Parallel execution finished\n");
}

void test_ycsb_e(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  uint64_t num_insert_total = num_ops * 0.05; // 5%插入（调用原test_load）
  uint64_t num_scan_total = num_ops * 0.95;   // 95%扫描（1线程）
  uint64_t scan_len = 100;                    // 扫描长度

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t insert_worker; // 1个插入线程（包装test_load）
  pthread_t scan_worker;   // 1个扫描线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // 插入（写）时间基准对齐

  // ========== 构造插入线程参数（传给包装函数） ==========
  typedef struct {
    algorithm *palgo;
    uint64_t base; // 起始 key
    uint64_t num;
    volatile bool *pstart;
  } InsertArgs;
  InsertArgs insert_args = {.palgo = palgo,
                            .base = pool_size + 1,
                            .num = num_insert_total,
                            .pstart = &start};

  // ========== 构造扫描线程参数 ==========
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    uint64_t scan_len;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } ScanArgs;
  ScanArgs scan_args = {.palgo = palgo,
                        .max = (int)(pool_size + num_insert_total + 1),
                        .num = (uint64_t)(num_scan_total / scan_len),
                        .scan_len = scan_len,
                        .is_zipf = true,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload e: Create 1 insert thread + 1 scan "
          "thread (parallel)\n");
  // 创建插入线程（写入新 key，不覆盖已加载数据）
  pthread_create(&insert_worker, NULL, test_insert_latest_tr, &insert_args);
  // 创建扫描线程
  pthread_create(&scan_worker, NULL, test_scan_tr, &scan_args);

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5); // 等待所有线程初始化完成
  ftl_log("YCSB Workload e: Start insert (test_load) + scan (parallel)\n");
  start = true;

  // ========== 等待所有线程执行完毕 ==========
  // 等待插入线程结束（test_load执行完成）
  pthread_join(insert_worker, NULL);
  ftl_log("YCSB Workload e: Insert thread (test_load) finished\n");
  // 等待扫描线程结束
  pthread_join(scan_worker, NULL);
  ftl_log("YCSB Workload e: Scan thread finished\n");
  ftl_log("YCSB Workload e: Parallel execution finished\n");
}

void test_ycsb_f(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed) {
  uint64_t num_rmw_total = num_ops * 0.5; // 50% RMW操作（1线程）
  uint64_t num_read_per_worker =
      (num_ops * 0.5) / nr_workers; // 50%读（多线程）

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t rmw_worker; // 1个RMW线程（替换原update_worker）
  pthread_t read_workers[nr_workers]; // nr_workers个读线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // RMW（写）时间基准对齐
  rd_start_ns = parallel_start_ns; // 读时间基准对齐

  // ========== 构造RMW线程参数（和原UpdateArgs一致） ==========
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } RmwArgs;
  RmwArgs rmw_args = {.palgo = palgo,
                      .max = pool_size,
                      .num = num_rmw_total, // RMW操作总数
                      .is_zipf = true,
                      .seed = seed,
                      .pstart = &start};

  // ========== 构造读线程参数（不变） ==========
  typedef struct {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload f: Create 1 RMW thread + %d read threads (parallel)\n",
          nr_workers);
  ycsb_rmw_running = true;
  pthread_create(&rmw_worker, NULL, test_rmw_tr, &rmw_args);
  for (int i = 0; i < nr_workers; ++i) {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5);
  ftl_log("YCSB Workload f: Start RMW (Read-Modify-Write) + read (parallel)\n");
  start = true;

  pthread_join(rmw_worker, NULL);
  ftl_log("YCSB Workload f: RMW thread finished\n");
  for (int i = 0; i < nr_workers; ++i) {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload f: All read threads finished\n");
  ftl_log("YCSB Workload f: Parallel execution finished\n");
  ycsb_rmw_running = false;
}
