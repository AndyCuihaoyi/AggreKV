#include "latency_manager.h"
#include "../lsmtree/lsmtree.h"

extern lsmtree LSM;

void lat_init()
{
    Lat.complete_r_slow = 0;
    Lat.complete_w_slow = 0;
    memset(Lat.rlat_arr, 0, MAX_LATENCY_US * sizeof(uint64_t));
    memset(Lat.wlat_arr, 0, MAX_LATENCY_US * sizeof(uint64_t));
}

void brief_fprintf(FILE *file)
{
    fprintf(file, "TotalSize: %ld GB\n", TOTALSIZE / G);
    fprintf(file, "ShowingSize: %ld GB\n", SHOWINGSIZE / G);
    fprintf(file, "Pinning Mem: %ld MB\n", LSM.lsp.pin_memory / M);
}

void rlat_print()
{
    FILE *file = fopen("/home/cuihaoyi/hash_dftl-nohost/output/rlat_output_10000000random_5_4_4.txt", "w");
    if (!file) {
        perror("Failed to open file");
        return;
    }

    long long total_lat = 0;
    uint64_t io_cnt = 0;
    brief_fprintf(file);

    for (int i = 0; i < MAX_LATENCY_US; i++)
    {
        if (Lat.rlat_arr[i])
        {
            total_lat += i * Lat.rlat_arr[i];
            io_cnt += Lat.rlat_arr[i];
            fprintf(file, "rlat: %d us; cnt: %ld\n", i, Lat.rlat_arr[i]);
        }
    }

    fclose(file);
}

void wlat_print()
{
    FILE *file = fopen("/home/cuihaoyi/hash_dftl-nohost/output/wlat_output_10000000random_5_4_4.txt", "w");
    if (!file) {
        perror("Failed to open file");
        return;
    }

    brief_fprintf(file);

    long long total_lat = 0;
    uint64_t io_cnt = 0;
    for (int i = 0; i < MAX_LATENCY_US; i++)
    {
        if (Lat.wlat_arr[i])
        {
            total_lat += i * Lat.wlat_arr[i];
            io_cnt += Lat.wlat_arr[i];
            fprintf(file, "wlat: %d us; cnt: %ld\n", i, Lat.wlat_arr[i]);
        }
    }

    fclose(file);
}

void mix_lat_print()
{
    FILE *file = fopen("/home/cuihaoyi/hash_dftl-nohost/output/ycsb_f.txt", "w");
    if (!file) {
        perror("Failed to open file");
        return;
    }

    for (int i = 0; i < MAX_LATENCY_US; i++)
    {
        if (Lat.wlat_arr[i])
        {
            fprintf(file, "wlat: %d us; cnt: %ld\n", i, Lat.wlat_arr[i]);
        }
        if (Lat.rlat_arr[i])
        {
            fprintf(file, "rlat: %d us; cnt: %ld\n", i, Lat.rlat_arr[i]);
        }
    }

    fclose(file);
}
