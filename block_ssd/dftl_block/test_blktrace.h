#ifndef __TEST_BLOCKTRACE_H__
#define __TEST_BLOCKTRACE_H__

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "../tools/blktrace/blktrace.h"

typedef struct algorithm algorithm;

typedef struct test_worker_args {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    int worker_id;
    blktrace_file_content *blktrace;
    bool is_blktrace_ramp;
    volatile bool *pstart;
} test_worker_args;

struct test_env {
    uint64_t pool_size;
    uint64_t num_update;
    uint64_t num_read;
    float map_size_frac;
    int seed;
    uint64_t ext_mem_lat;

    // for blktrace replay
    char *blktrace_file;
    uint64_t num_upper_level_kvops;
} test_env;

struct option env_opts[] = {
    {"pool_size", required_argument, NULL, 0},
    {"num_update", required_argument, NULL, 1},
    {"num_read", required_argument, NULL, 2},
    {"map_size_frac", required_argument, NULL, 3},
    {"seed", required_argument, NULL, 4},
    {"ext_mem_lat", required_argument, NULL, 5},
    {"blktrace_file", required_argument, NULL, 6},
    {"help", no_argument, NULL, 7},
    {"num_upper_level_kvops", required_argument, NULL, 8},
    {0, 0, 0, 0},
};

#endif // __TEST_BLOCKTRACE_H__
