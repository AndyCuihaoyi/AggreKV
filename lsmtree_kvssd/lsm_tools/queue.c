#include <stdlib.h>
#include <stdio.h>
#include "queue.h"
#include "container.h"
#include "skiplist.h"
#include "../lsmtree/lsm_utils.h"

void q_init(queue **q, int qsize)
{
    *q = (queue *)malloc(sizeof(queue));
    (*q)->size = 0;
    (*q)->head = (*q)->tail = NULL;
    pthread_mutex_init(&((*q)->q_lock), NULL);
    (*q)->firstFlag = true;
    (*q)->m_size = qsize;
}

bool q_enqueue(void *req, queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (q->size == q->m_size)
    {
        pthread_mutex_unlock(&q->q_lock);
        return false;
    }

    node *new_node = (node *)malloc(sizeof(node));
    new_node->d.req = req;
    new_node->next = NULL;
    if (q->size == 0)
    {
        q->head = q->tail = new_node;
    }
    else
    {
        q->tail->next = new_node;
        q->tail = new_node;
    }
    q->size++;
    pthread_mutex_unlock(&q->q_lock);
    return true;
}

bool q_enqueue_front(void *req, queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (q->size == q->m_size)
    {
        pthread_mutex_unlock(&q->q_lock);
        return false;
    }
    node *new_node = (node *)malloc(sizeof(node));
    new_node->d.req = req;
    new_node->next = NULL;
    if (q->size == 0)
    {
        q->head = q->tail = new_node;
    }
    else
    {
        new_node->next = q->head;
        q->head = new_node;
    }
    //	printf("ef-key:%u\n",((request*)req)->key);
    q->size++;
    pthread_mutex_unlock(&q->q_lock);
    return true;
}

void *q_dequeue(queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (!q->head || q->size == 0)
    {
        pthread_mutex_unlock(&q->q_lock);
        return NULL;
    }
    node *target_node;
    target_node = q->head;
    q->head = q->head->next;

    void *res = target_node->d.req;
    q->size--;
    //	printf("of-key:%u\n",((request*)res)->key);
    free(target_node);
    pthread_mutex_unlock(&q->q_lock);
    return res;
}

void *q_pick(queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (!q->head || q->size == 0)
    {
        pthread_mutex_unlock(&q->q_lock);
        return NULL;
    }
    node *target_node;
    target_node = q->head;
    void *res = target_node->d.req;
    pthread_mutex_unlock(&q->q_lock);
    return res;
}

void q_free(queue *q)
{
    while (q_dequeue(q))
    {
    }
    pthread_mutex_destroy(&q->q_lock);
    free(q);
}

bool q_enqueue_int(int req, queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (q->size == q->m_size)
    {
        pthread_mutex_unlock(&q->q_lock);
        return false;
    }

    node *new_node = (node *)malloc(sizeof(node));
    new_node->d.data = req;
    new_node->next = NULL;
    if (q->size == 0)
    {
        q->head = q->tail = new_node;
    }
    else
    {
        q->tail->next = new_node;
        q->tail = new_node;
    }
    q->size++;
    pthread_mutex_unlock(&q->q_lock);
    return true;
}

int q_dequeue_int(queue *q)
{
    pthread_mutex_lock(&q->q_lock);
    if (!q->head || q->size == 0)
    {
        pthread_mutex_unlock(&q->q_lock);
        return 0;
    }
    node *target_node;
    target_node = q->head;
    q->head = q->head->next;

    int res = target_node->d.data;
    q->size--;
    //	printf("of-key:%u\n",((request*)res)->key);
    free(target_node);
    pthread_mutex_unlock(&q->q_lock);
    return res;
}

algo_q *algo_q_create()
{
    algo_q *q = malloc(sizeof(algo_q));
    q->head = q->last = NULL;
    q->size = 0;
    return q;
}

void algo_q_free(algo_q *q)
{
    while (q->head)
    {
        algo_q_node *tmp = q->head;
        q->head = q->head->next;
        free(tmp);
    }
    q->last = NULL;
    free(q);
}

void algo_q_insert_sorted(algo_q *q, void *req, void *wbe)
{
    /**
     * Requests are placed in @work_queue sorted by their target time.
     * @work_queue is statically allocated and the ordered list is
     * implemented by chaining the indexes of entries with @prev and @next.
     * This implementation is nasty but we do this way over dynamically
     * allocated linked list to minimize the influence of dynamic memory
     * allocation. Also, this O(n) implementation can be improved to O(logn)
     * scheme with e.g., red-black tree but....
     */

    if (!q)
    {
        abort();
    }

    if (q->head == NULL)
    {
        ftl_assert(q->last == NULL);
        algo_q_node *entry = malloc(sizeof(algo_q_node));
        if (req)
        {
            entry->payload = req;
        }
        else if (wbe)
        {
            entry->payload = wbe;
        }
        else
        {
            abort();
        }
        entry->prev = entry->next = NULL;
        q->head = q->last = entry;
        ftl_assert(q->head->prev == NULL);
    }
    else
    {
        uint64_t nsecs_target = 0;
        algo_q_node *curr = q->last;
        algo_q_node *entry = malloc(sizeof(algo_q_node));
        entry->prev = entry->next = NULL;
        if (req)
        {
            nsecs_target = ((request *)req)->etime + 1000; // 1us
            entry->payload = req;
        }
        else if (wbe)
        {
            nsecs_target = ((snode *)wbe)->etime + 1000; // 1us
            entry->payload = wbe;
        }
        else
        {
            abort();
        }

        if (req)
        {
            while (curr != NULL)
            {
                if (((request *)curr->payload)->etime <= ((request *)req)->etime)
                    break;

                if (((request *)curr->payload)->etime <= nsecs_target)
                    break;

                curr = curr->prev;
            }
        }
        else if (wbe)
        {
            while (curr != NULL)
            {
                if (((snode *)curr->payload)->etime <= ((snode *)wbe)->etime)
                    break;

                if (((snode *)curr->payload)->etime <= nsecs_target)
                    break;

                curr = curr->prev;
            }
        }
        else
        {
            abort();
        }

        if (curr == NULL)
        { /* Head inserted */
            entry->prev = NULL;
            q->head->prev = entry;
            entry->next = q->head;
            q->head = entry;
            ftl_assert(q->head->next != NULL);
        }
        else if (curr->next == NULL)
        { /* Tail */
            entry->prev = curr;
            entry->next = curr->next;
            curr->next = entry;
            q->last = entry;
        }
        else
        { /* In between */
            entry->prev = curr;
            entry->next = curr->next;
            curr->next->prev = entry;
            curr->next = entry;
        }
    }
    q->size++;
}

void *algo_q_dequeue(algo_q *q)
{
    if (!q)
    {
        abort();
    }

    if (q->head == NULL)
    {
        return NULL;
    }

    algo_q_node *entry = q->head;
    q->head = q->head->next;
    if (q->head)
    {
        q->head->prev = NULL;
    }
    void *payload = entry->payload;

    if (q->head == NULL)
    {
        q->last = NULL;
    }
    q->size--;

    free(entry);
    return payload;
}
