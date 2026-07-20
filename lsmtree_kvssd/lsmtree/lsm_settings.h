#ifndef __H_SETTING__
#define __H_SETTING__
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// basic unit
#define K (1024)
#define M (1024 * K)
#define G (1024 * M)
#define T (1024L * G)
#define P (1024L * T)
#define MILI (1000000)

// basic logic unit
typedef uint32_t ppa_t;
typedef uint32_t lpa_t;
#define PIECE 512                         // page is divied into piece
#define PIECE_PER_PAGE (PAGESIZE / PIECE) // num of piece per page
#define MINVALUE PIECE                    // min value size
#define MINKEYLENGTH 16                   // min key size
#define MAXKEYLENGTH 255                  // max key size
#define DEFKEYLENGTH 32                   // avg key size
#define DEFVALUESIZE (1024)               // default value seize 1K
#define KEYSETSIZE 8
// basic physic volumn
#define TOTALSIZE (64L * G)
#define OP 70 // 70% op
#define SHOWINGSIZE (TOTALSIZE / 100 * OP)

// for superblock and page manage
#define POINTER_SIZE 4
#define PAGESIZE (4 * K)
#define KEYBITMAP (1 * K)
#define MAXKEY_IN_METAPAGE                                                     \
  (PAGESIZE / (DEFKEYLENGTH + POINTER_SIZE)) // threshold num for metapage
#define _NOP_NO_OP ((1ULL << 26))            // 256GB LPA space (4M pages)
#define _NOP ((_NOP_NO_OP * 115ULL) / 100)   // 15% OP for logical space
#define _PPS (1ULL << 16) // 256MB (匹配 pgs_per_line=65536)
#define _NOS (_NOP / _PPS)

// for part
#define PARTNUM 2 // DATA and MAP
#define KEYLEN(a) (a.len + sizeof(ppa_t))

// for lsmtree
#define DEF_LEVEL 5
#define DEF_TOP_K_LEVEL 3
#define DEF_CXL_LEVEL 0

typedef struct str_key {
  uint8_t len;
  char *key;
} str_key;
#define KEYT str_key

#define KEYFORMAT(input)                                                       \
  input.len > DEFKEYLENGTH ? DEFKEYLENGTH : input.len, input.key

/*
    return comparation res between two keys:
    0 for equal
    neg for a<b
    pos for a>b
*/
static inline int KEYCMP(KEYT a, KEYT b) {
  if (!a.len && !b.len)
    return 0;
  else if (a.len == 0)
    return -1;
  else if (b.len == 0)
    return 1;

  int r = memcmp(a.key, b.key, a.len > b.len ? b.len : a.len);
  if (r != 0 || a.len == b.len) {
    return r;
  }
  return a.len < b.len ? -1 : 1;
}

// return comparation res between key and const char*:
static inline int KEYCONSTCOMP(KEYT a, char *s) {
  int len = strlen(s);
  if (!a.len && !len)
    return 0;
  else if (a.len == 0)
    return -1;
  else if (len == 0)
    return 1;

  int r = memcmp(a.key, s, a.len > len ? len : a.len);
  if (r != 0 || a.len == len) {
    return r;
  }
  return a.len < len ? -1 : 1;
}

// exact comparation
static inline char KEYTEST(KEYT a, KEYT b) {
  if (a.len != b.len)
    return 0;
  return memcmp(a.key, b.key, a.len) ? 0 : 1;
}

// check key invalidation
static inline bool KEYVALCHECK(KEYT a) {
  if (a.len <= 0)
    return false;
  if (a.key[0] < 0)
    return false;
  return true;
}

#define BLOCKT uint32_t
#define V_PTR char *const
#define PTR char *
#define QSIZE (1024)
#define LOWQDEPTH (64)
#define QDEPTH (64)

#define THPOOL
#define NUM_THREAD 1

void lsm_setup_params();
float get_sizefactor(uint32_t keynum_in_header);
void header_volumn_print(float size_factor);
#endif
