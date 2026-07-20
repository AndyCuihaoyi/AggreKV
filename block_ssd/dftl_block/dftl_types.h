/**
 * @file dftl_types.h
 * @brief DFTL Core Type Definitions
 *
 * Defines all fundamental data structures used in the DFTL implementation:
 * - Address types (PPA, LPA, fingerprint)
 * - Request and value set structures
 * - Algorithm interface
 * - State machines and enums
 */

#ifndef DFTL_TYPES_H
#define DFTL_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "dftl_settings.h"
#include "../tools/rte_ring/rte_ring.h"
#include <pthread.h>
#include "algo_queue.h"

#define KEYT str_key
#define PTR char *

typedef struct queue queue;

/*============================================================================
 * Address Types
 *============================================================================*/

/** Physical Page Address - points to a page in NAND flash */
typedef uint32_t ppa_t;

/** Logical Page Address - user-visible address space */
typedef uint32_t lpa_t;

/** Fingerprint - hash of key for bloom filter / duplicate detection */
typedef uint32_t fp_t;

typedef struct snode snode;

/*============================================================================
 * Key-Value Types
 *============================================================================*/

/**
 * @brief Key structure for KV pairs
 */
typedef struct str_key
{
    uint8_t len;                    /**< Actual key length */
    char key[MAXKEYSIZE];           /**< Key content */
} str_key;

/**
 * @brief Prefill entry for bulk loading
 * Used during initialization to pre-populate the mapping
 */
typedef struct prefill_type
{
    lpa_t lpa;                     /**< Logical page address */
    KEYT key;                      /**< Key for this entry */
} prefill_t;

/**
 * @brief Value set for KV pair storage
 */
typedef struct value_set
{
    PTR value;                      /**< Pointer to value data */
    uint32_t length;              /**< Length in grains */
    uint32_t ppa;                 /**< Physical page address */
    uint32_t length_in_bytes;     /**< Length in bytes */
    uint32_t offset;               /**< Offset within a page */
} value_set;

typedef struct lower_info lower_info;

/*============================================================================
 * Hash and Lookup Types
 *============================================================================*/

/**
 * @brief Parameters for hash-based key lookup
 */
typedef struct hash_params
{
    uint32_t hash;                 /**< Hash value for index */
#ifdef STORE_KEY_FP
    fp_t key_fp;                   /**< Key fingerprint */
#endif
    int cnt;                       /**< Collision chain length */
    int find;                      /**< Found position */
} hash_params;

/*============================================================================
 * Request State Machine
 *============================================================================*/

/**
 * @brief Request jump targets for state machine
 */
typedef enum
{
    GOTO_LOAD,      /**< Load translation page */
    GOTO_LIST,      /**< Add to cache list */
    GOTO_EVICT,     /**< Evict from cache */
    GOTO_COMPLETE,  /**< Complete request */
    GOTO_READ,     /**< Read data page */
    GOTO_WRITE,    /**< Write data page */
    GOTO_UPDATE,   /**< Update mapping */
} jump_t;

/**
 * @brief In-flight request parameters
 */
typedef struct inflight_params
{
    jump_t jump;                    /**< Next state */
} inflight_params;

/**
 * @brief Request completion state
 */
typedef enum req_state_t
{
    ALGO_REQ_PENDING = 0,   /**< Request in progress */
    ALGO_REQ_NOT_FOUND = 1, /**< Key not found */
} req_state_t;

/*============================================================================
 * Performance Measurement
 *============================================================================*/

/**
 * @brief Timing breakdown for latency analysis
 */
enum inner_time_t
{
    SQ_SUBMIT = 0,      /**< Queue submission */
    META_LI_RD = 1,    /**< Mapping read latency */
    META_LI_WR = 2,     /**< Mapping write latency */
    DATA_LI_RD = 3,     /**< Data read latency */
    DATA_LI_WR = 4,     /**< Data write latency */
    CQ_COMPLETE = 5,    /**< Completion processing */
    REQ_TT_LAT = 6,     /**< Total latency */

    INNER_TIMER_SIZE,
};

/**
 * @brief Per-request timer
 */
struct req_inner_timer
{
    bool is_start;
    uint64_t start;
    uint64_t elapsed;
};

/*============================================================================
 * Request Structure
 *============================================================================*/

/**
 * @brief I/O request structure
 */
typedef struct request
{
    uint8_t type;                  /**< Request type */
    KEYT key;                     /**< Key */
    value_set *value;             /**< Value data */
    hash_params *h_params;       /**< Hash parameters */
    req_state_t state;            /**< Request state */
    uint64_t stime;              /**< Start time */
    uint64_t etime;              /**< End time */
    struct req_inner_timer inner_timer[INNER_TIMER_SIZE]; /**< Latency breakdown */
    pthread_spinlock_t timer_lock; /**< Timer lock */

    inflight_params *params;      /**< State machine params */

    volatile int *ptr_nr_ios;     /**< IO depth counter */

    void *(*end_req)(struct request *const); /**< Completion callback */
} request;

/*============================================================================
 * Algorithm Interface
 *============================================================================*/

/**
 * @brief Parameters for demand paging
 */
typedef struct demand_params
{
    value_set *value;
    snode *wb_entry;
    int offset;
} demand_params;

/**
 * @brief Algorithm interface (virtual function table)
 */
typedef struct algorithm
{
    /* Lifecycle */
    uint32_t (*argument_set)(int argc, char **argv);
    uint32_t (*create)(struct algorithm *, lower_info *);
    void (*destroy)(struct algorithm *, lower_info *);

    /* Basic operations */
    uint32_t (*read)(struct algorithm *, request *const);
    uint32_t (*write)(struct algorithm *, request *const);
    uint32_t (*remove)(struct algorithm *, request *const);

#ifdef KVSSD
    /* Iterator operations */
    uint32_t (*iter_create)(struct algorithm *, request *const);
    uint32_t (*iter_next)(struct algorithm *, request *const);
    uint32_t (*iter_next_with_value)(struct algorithm *, request *const);
    uint32_t (*iter_release)(struct algorithm *, request *const);
    uint32_t (*iter_all_key)(struct algorithm *, request *const);
    uint32_t (*iter_all_value)(struct algorithm *, request *const);

    /* Batch operations */
    uint32_t (*multi_set)(struct algorithm *, request *const, int num);
    uint32_t (*multi_get)(struct algorithm *, request *const, int num);
    uint32_t (*range_query)(struct algorithm *, request *const);
#endif

    /* Queues */
    struct rte_ring *req_q;       /**< Input queue */
    algo_q *retry_q;              /**< Retry queue */
    struct rte_ring *finish_q;     /**< Completion queue */

    lower_info *li;               /**< Flash interface */

    void *env;                    /**< Private data */
} algorithm;

#endif /* DFTL_TYPES_H */
