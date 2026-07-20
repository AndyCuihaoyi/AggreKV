#ifndef __H_LAT_MANAGER_
#define __H_LAT_MANAGER_
#include "../lsmtree/lsm_settings.h"
#define MAX_LATENCY_US 10000000 // 10s
#define CXL_DIR_LAT 300
#define CXL_SWITCH_LAT 800
typedef struct lat_mgr
{
    uint64_t wlat_arr[MAX_LATENCY_US];
    uint64_t rlat_arr[MAX_LATENCY_US];
    uint64_t complete_r_slow;
    uint64_t complete_w_slow;
} lat_mgr;
lat_mgr Lat;
void lat_init();
void rlat_print();
void wlat_print();
void mix_lat_print();
#endif //__H_LAT_MANAGERE_
