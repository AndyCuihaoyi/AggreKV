#include "lru_list.h"
#include "../hash_hot_cmt/demand.h"

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

static int lru_top_pct_window(int total, int pct)
{
	int window = (total * pct + 99) / 100;

	return window > 0 ? window : 1;
}

static int lru_top_k_min_heat(uint32_t heats[], int k, int count)
{
	int i;
	int min_idx = 0;

	if (count == 0)
		return -1;
	for (i = 1; i < count && i < k; i++)
	{
		if (heats[i] < heats[min_idx])
			min_idx = i;
	}
	return min_idx;
}

void lru_pick_promote_hot_pages(LRU *lru, lru_promote_result_t *out, int target_k)
{
	NODE *current;
	int traverse_cnt = 0;
	int top_window;

	if (out == NULL)
		return;
	out->count = 0;
	if (lru == NULL || lru->size == 0)
		return;
	if (target_k <= 0 || target_k > LRU_PROMOTE_MAX_K)
		return;

	top_window = lru_top_pct_window(lru->size, COLD_LRU_PROMOTE_TOP_PCT);
	current = lru->head;
	while (current != NULL && traverse_cnt < top_window)
	{
		if (out->count >= target_k)
			break;
		if (current->DATA != NULL)
		{
			struct cmt_struct *cmt = (struct cmt_struct *)(current->DATA);
			if (cmt != NULL && cmt->has_hot_entry
#ifdef ADAPTIVE_MEM
			    && !cmt_is_ghost(cmt)
#endif
			) {
				out->candidates[out->count] = current->DATA;
				out->count++;
			}
		}
		current = current->next;
		traverse_cnt++;
	}
}

void lru_pick_promote_top_k(LRU *lru, lru_promote_result_t *out, int k)
{
	NODE *current;
	uint32_t heats[LRU_PROMOTE_MAX_K];
	int traverse_cnt = 0;
	int top_window;
	int min_idx;

	if (out == NULL)
		return;
	out->count = 0;
	if (lru == NULL || lru->size == 0)
		return;
	if (k > LRU_PROMOTE_MAX_K)
		k = LRU_PROMOTE_MAX_K;
	if (k <= 0)
		return;

	top_window = lru_top_pct_window(lru->size, COLD_LRU_PROMOTE_TOP_PCT);
	current = lru->head;
	while (current != NULL && traverse_cnt < top_window)
	{
		if (current->DATA != NULL)
		{
			struct cmt_struct *cmt = (struct cmt_struct *)(current->DATA);
			uint32_t page_heat = 0;

			if (cmt != NULL && cmt->cnt_map != NULL)
				page_heat = cmt->page_heat_sum;

			if (page_heat > 0)
			{
				if (out->count < k)
				{
					heats[out->count] = page_heat;
					out->candidates[out->count] = current->DATA;
					out->count++;
				}
				else
				{
					min_idx = lru_top_k_min_heat(heats, k, out->count);
					if (min_idx >= 0 && page_heat > heats[min_idx])
					{
						heats[min_idx] = page_heat;
						out->candidates[min_idx] = current->DATA;
					}
				}
			}
		}
		current = current->next;
		traverse_cnt++;
	}
}

void *lru_pick_promote_top_heat(LRU *lru)
{
	lru_promote_result_t res;

	lru_pick_promote_top_k(lru, &res, 1);
	return res.count > 0 ? res.candidates[0] : NULL;
}

void *lru_pop_hot(LRU *lru)
{
	NODE *current;
	NODE *target_node = NULL;
	void *target_data;
	uint32_t max_page_heat = 0;
	int traverse_cnt = 0;
	int top_window;

	if (lru == NULL || lru->size == 0)
		return NULL;

	top_window = lru_top_pct_window(lru->size, COLD_LRU_PROMOTE_TOP_PCT);
	current = lru->head;
	while (current != NULL && traverse_cnt < top_window)
	{
		if (current->DATA != NULL)
		{
			struct cmt_struct *cmt = (struct cmt_struct *)(current->DATA);
			uint32_t page_heat = 0;

			if (cmt != NULL && cmt->cnt_map != NULL)
				page_heat = cmt->page_heat_sum;

			if (page_heat > max_page_heat)
			{
				max_page_heat = page_heat;
				target_node = current;
			}
		}
		current = current->next;
		traverse_cnt++;
	}

	if (target_node == NULL)
		return NULL;

	target_data = target_node->DATA;

	if (target_node == lru->head)
	{
		lru->head = target_node->next;
		if (lru->head != NULL)
			lru->head->prev = NULL;
		else
			lru->tail = NULL;
	}
	else if (target_node == lru->tail)
	{
		lru->tail = target_node->prev;
		if (lru->tail != NULL)
			lru->tail->next = NULL;
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