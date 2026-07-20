#ifndef __H_QUEUE_Q_
#define __H_QUEUE_Q_
#include "../lsmtree/lsm_settings.h"
#include <pthread.h>
#define for_each_intqueue(q, a) \
    for (a = q_dequeue_int(q);  \
         a != -1;               \
         a = q_dequeue_int(q))

#define for_each_rqueue(q, a) \
    for (a = q_dequeue(q);    \
         a != NULL;           \
         a = q_dequeue(q))

#define for_each_rqueue_type(q, a, type) \
    for (a = (type)q_dequeue(q);         \
         a != NULL;                      \
         a = (type)q_dequeue(q))

typedef struct node
{
    union
    {
        void *req;
        int data;
    } d;
    struct node *prev;
    struct node *next;
} node;

typedef struct queue
{
    volatile int size;
    int m_size;
    pthread_mutex_t q_lock;
    bool firstFlag;
    node *head;
    node *tail;
} queue;

typedef struct algo_queue_node
{
    struct algo_queue_node *prev, *next;
    void *payload;
} algo_q_node;

typedef struct algo_queue
{
    algo_q_node *head, *last;
    uint64_t size;
} algo_q;

void q_init(queue **, int);
bool q_enqueue(void *, queue *);
bool q_enqueue_front(void *, queue *);
void *q_pick(queue *);
void q_lock(queue *);
void q_unlock(queue *);
void *q_dequeue(queue *);
void q_free(queue *);
bool q_enqueue_int(int, queue *);
int q_dequeue_int(queue *);

algo_q *algo_q_create();
void algo_q_insert_sorted(algo_q *q, void *req, void *wbe);
void *algo_q_dequeue(algo_q *q);
void algo_q_free(algo_q *q);

#endif
