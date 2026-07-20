#include "lru_list.h"
#include "../dftl_block/demand.h"

void lru_init(LRU **lru)
{
	*lru = (LRU *)malloc(sizeof(LRU));
	(*lru)->size = 0;
	(*lru)->head = (*lru)->tail = NULL;
}

void lru_free(LRU *lru)
{
	while (lru_pop(lru))
	{
	}
	free(lru);
}

NODE *lru_push(LRU *lru, void *table_ptr)
{
	NODE *now = (NODE *)malloc(sizeof(NODE));
	now->DATA = table_ptr;
	now->next = now->prev = NULL;
	if (lru->size == 0)
	{
		lru->head = lru->tail = now;
	}
	else
	{
		lru->head->prev = now;
		now->next = lru->head;
		lru->head = now;
	}
	lru->size++;
	return now;
}

void *lru_pop(LRU *lru)
{
	if (!lru->head || lru->size == 0)
	{
		return NULL;
	}
	NODE *now = lru->tail;
	void *re = now->DATA;
	lru->tail = now->prev;
	if (lru->tail != NULL)
	{
		lru->tail->next = NULL;
	}
	else
	{
		lru->head = NULL;
	}
	lru->size--;
	free(now);
	return re;
}

void *lru_pop_hot(LRU *lru)
{
	if (lru == NULL || lru->size == 0)
	{
		return NULL;
	}

	int total = lru->size;
	int top_10_percent = (total * 10 + 99) / 100; // 向上取整公式：(a*b + 99)/100
	if (top_10_percent <= 0)
	{
		top_10_percent = 1;
	}

	NODE *current = lru->head;
	NODE *target_node = NULL;
	uint32_t max_hit_cnt = 0;
	int traverse_cnt = 0;

	while (current != NULL && traverse_cnt < top_10_percent)
	{
		if (current->DATA != NULL)
		{
			struct cmt_struct *cmt = (struct cmt_struct *)(current->DATA);
			if (cmt->hit_cnt > max_hit_cnt)
			{
				max_hit_cnt = cmt->hit_cnt;
				target_node = current;
			}
		}
		current = current->next;
		traverse_cnt++;
	}

	if (target_node == NULL)
	{
		return NULL;
	}

	void *target_data = target_node->DATA;

	if (target_node == lru->head)
	{
		lru->head = target_node->next;
		if (lru->head != NULL)
		{
			lru->head->prev = NULL;
		}
		else
		{
			lru->tail = NULL;
		}
	}
	else if (target_node == lru->tail)
	{
		lru->tail = target_node->prev;
		if (lru->tail != NULL)
		{
			lru->tail->next = NULL;
		}
	}
	else
	{
		target_node->prev->next = target_node->next;
		target_node->next->prev = target_node->prev;
	}

	lru->size--;
	free(target_node);

	return target_data;
}

void lru_update(LRU *lru, NODE *now)
{
	if (now == NULL)
	{
		return;
	}
	if (now == lru->head)
	{
		return;
	}
	if (now == lru->tail)
	{
		lru->tail = now->prev;
		lru->tail->next = NULL;
	}
	else
	{
		now->prev->next = now->next;
		now->next->prev = now->prev;
	}
	now->prev = NULL;
	lru->head->prev = now;
	now->next = lru->head;
	lru->head = now;
}

void lru_delete(LRU *lru, NODE *now)
{
	if (now == NULL)
	{
		return;
	}
	if (now == lru->head)
	{
		lru->head = now->next;
		if (lru->head != NULL)
		{
			lru->head->prev = NULL;
		}
		else
		{
			lru->tail = NULL;
		}
	}
	else if (now == lru->tail)
	{
		lru->tail = now->prev;
		lru->tail->next = NULL;
	}
	else
	{
		now->prev->next = now->next;
		now->next->prev = now->prev;
	}
	lru->size--;
	free(now);
}

NODE *lru_get_target_node(LRU *lru, NODE *now)
{
	if (lru != NULL)
	{
		return lru->head;
	}

	if (now != NULL)
	{
		return now->next;
	}

	return NULL;
}