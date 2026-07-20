#include "zipf.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    struct zipf_state zs;
    uint64_t nranges = 40000000;
    uint64_t *cnt = malloc(sizeof(uint64_t) * nranges);
    for (int i = 0; i < nranges; i++) {
        cnt[i] = 0;
    }
    zipf_init(&zs, nranges, 0.99, 0, 1);
    // zipf_disable_hash(&zs);
    for (uint64_t i = 0; i < 10000000; i++) {
        cnt[zipf_next(&zs)]++;
    }
    for (uint64_t i = 0; i < nranges; i++) {
        if (cnt[i] > 0)
            printf("%ld: %ld\n", i, cnt[i]);
    }
    free(cnt);
    return 0;
}