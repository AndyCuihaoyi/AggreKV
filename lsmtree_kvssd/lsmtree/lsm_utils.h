#ifndef LSM_UTILS_H
#define LSM_UTILS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <bits/time.h>
#include <bits/time64.h>
#include "lsm_settings.h"
extern uint64_t timer_start_ns;
#define clock_get_ns()                            \
    ({                                            \
        struct timespec ts;                       \
        clock_gettime(CLOCK_REALTIME, &ts);       \
        ((uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec); \
    })
#define ftl_log(fmt, ...)                                                                               \
    do                                                                                                  \
    {                                                                                                   \
        uint64_t time_ns = clock_get_ns();                                                              \
        fprintf(stdout, "[%.6lf] [LOG] " fmt, (double)(time_ns - timer_start_ns) / 1e9, ##__VA_ARGS__); \
    } while (0)

#define ftl_err(fmt, ...)                                                                                                           \
    do                                                                                                                              \
    {                                                                                                                               \
        uint64_t time_ns = clock_get_ns();                                                                                          \
        fprintf(stderr, "[%.6lf] [ERROR] %s:%d:" fmt, (double)(time_ns - timer_start_ns) / 1e9, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#ifdef DEBUG_FTL
#define ftl_debug(fmt, ...)                                                                               \
    do                                                                                                    \
    {                                                                                                     \
        uint64_t time_ns = clock_get_ns();                                                                \
        fprintf(stdout, "[%.6lf] [DEBUG] " fmt, (double)(time_ns - timer_start_ns) / 1e9, ##__VA_ARGS__); \
    } while (0)

#define ftl_assert(expr) \
    do                   \
    {                    \
        assert(expr);    \
    } while (0)
#else
#define ftl_debug(fmt, ...) \
    do                      \
    {                       \
    } while (0)

#define ftl_assert(expr) \
    do                   \
    {                    \
    } while (0)
#endif // DEBUG_FTL

char *kvssd_tostring(KEYT);
void kvssd_cpy_key(KEYT *des, KEYT *src);
void kvssd_free_key(KEYT *des);
void htable_print(char *target);
bool keyset_check(char *target);
#endif