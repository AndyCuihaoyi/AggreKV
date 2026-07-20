#define DVALUE
// #define DEBUG_FTL

#define MAX_INF_REQS (65536)

#define MAX_HASH_COLLISION (1024)

#define KVSSD
#define MAXKEYSIZE 32 // in bytes
#define MAX_WRITE_BUF 256

#define K 1024
#define M (1024 * K)
#define G (1024 * M)
#ifdef BLOCK_SSD
#define PIECE 4096
#else
#define PIECE 1024
#endif
#define GRAINED_UNIT PIECE // 1024
#define PAGESIZE 4096
#define GRAIN_PER_PAGE (PAGESIZE / GRAINED_UNIT) // 4
#define NPCINPAGE (PAGESIZE / PIECE)             // 4

#define _NOP_NO_OP (68157440)          // 260GB logical space
// #define _NOP_NO_OP ((1ULL << 26))          // 256GB logical space
// #define _NOP ((_NOP_NO_OP * 115ULL) / 100) // 15% OP for logical space
#define _NOP (154402816)                    // very large OP
#define _PPS (1ULL << 16)                  // 256MB
#define _NOS (_NOP / _PPS)

#define STORE_KEY_FP
// #define UPDATE_DATA_CHECK

// #define CMT_USE_NUMA
