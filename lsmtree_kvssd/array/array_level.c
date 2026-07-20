#include "array.h"
#include "../lsmtree/lsm_utils.h"
#include "../lsmtree/lsmtree.h"
extern lsmtree LSM;

void array_tier_align(level *lev)
{
	printf("this is empty\n");
}

bool array_chk_overlap(level *lev, KEYT start, KEYT end)
{
	if (KEYCMP(lev->start, end) > 0 || KEYCMP(lev->end, start) < 0)
	{
		return false;
	}
	return true;
}

run_t *array_range_find_lowerbound(level *lev, KEYT target)
{
	array_body *b = (array_body *)lev->level_data;
	run_t *arrs = b->arrs;
	int target_idx = array_bound_search(arrs, lev->n_num, target, true);
	if (target_idx == -1)
		return NULL;
	return &arrs[target_idx];
}

htable *array_mem_cvt2table(skiplist *mem, run_t *input)
{
	htable *res = htable_assign(NULL);

	input->cpt_data = res;
	snode *temp;
	char *ptr = (char *)res->sets;
	uint16_t *bitmap = (uint16_t *)ptr;
	uint32_t idx = 1;
	memset(bitmap, -1, KEYBITMAP / sizeof(uint16_t));
	uint16_t data_start = KEYBITMAP;
	bitmap[0] = mem->size;
	for_each_sk(temp, mem)
	{
		if (idx == 1)
		{
			kvssd_cpy_key(&input->key, &temp->key);
		}
		else if (idx == mem->size)
		{
			kvssd_cpy_key(&input->end, &temp->key);
		}
		memcpy(&ptr[data_start], &temp->ppa, sizeof(temp->ppa));
		memcpy(&ptr[data_start + sizeof(temp->ppa)], temp->key.key, temp->key.len);
		bitmap[idx] = data_start;
		data_start += temp->key.len + sizeof(temp->ppa);
		idx++;
	}
	bitmap[idx] = data_start;
	// htable_print((char *)(res->sets));
	return res;
}
// static int merger_cnt;

static char *make_rundata_from_snode(snode *temp)
{
	char *res = (char *)malloc(PAGESIZE);
	char *ptr = res;
	uint16_t *bitmap = (uint16_t *)ptr;
	uint32_t idx = 1;
	memset(bitmap, -1, KEYBITMAP / sizeof(uint16_t));
	uint16_t data_start = KEYBITMAP;
	uint32_t length = 0;
	do
	{
		memcpy(&ptr[data_start], &temp->ppa, sizeof(temp->ppa));
		memcpy(&ptr[data_start + sizeof(temp->ppa)], temp->key.key, temp->key.len);
		bitmap[idx] = data_start;

		data_start += temp->key.len + sizeof(temp->ppa);
		length += KEYLEN(temp->key);
		idx++;
		temp = temp->list[1];
	} while (temp && length + KEYLEN(temp->key) <= PAGESIZE - KEYBITMAP);
	bitmap[0] = idx - 1;
	bitmap[idx] = data_start;
	return res;
}

void array_header_print(char *data)
{
	int idx;
	KEYT key;
	ppa_t *ppa;
	uint16_t *bitmap;
	char *body;

	body = data;
	bitmap = (uint16_t *)body;
	printf("header_num:%d : %p\n", bitmap[0], data);
	for_each_header_start(idx, key, ppa, bitmap, body)
		fprintf(stderr, "[%d:%d] key(%p):%.*s(%d) ,%u\n", idx, bitmap[idx], &data[bitmap[idx]], key.len, key.key, key.len, *ppa);
	for_each_header_end
		printf("header_num:%d : %p\n", bitmap[0], data);
}

run_t *array_next_run(level *lev, KEYT key)
{
	array_body *b = (array_body *)lev->level_data;
	run_t *arrs = b->arrs;
	int target_idx = array_binary_search(arrs, lev->n_num, key);
	if (target_idx == -1)
		return NULL;
	if (target_idx + 1 < lev->n_num)
	{
		return &arrs[target_idx + 1];
	}
	return NULL;
}

typedef struct header_iter
{
	char *header_data;
	uint32_t idx;
} header_iter;

keyset_iter *array_header_get_keyiter(level *lev, char *data, KEYT *key)
{
	keyset_iter *res = (keyset_iter *)malloc(sizeof(keyset_iter));
	header_iter *p_data = (header_iter *)malloc(sizeof(header_iter));
	res->private_data = (void *)p_data;
	if (!data)
	{
		array_body *b = (array_body *)lev->level_data;
		run_t *arrs = b->arrs;
		int target = array_binary_search(arrs, lev->n_num, *key);
		if (target == -1)
			p_data->header_data = NULL;
		else
		{
			p_data->header_data = arrs[target].level_caching_data;
		}
	}
	else
	{
		p_data->header_data = data;
	}
	data = p_data->header_data;
	if (key == NULL)
		p_data->idx = 0;
	else if (data)
		p_data->idx = array_find_idx_lower_bound(data, *key);
	else
		return NULL;

	return res;
}

keyset array_header_next_key(level *lev, keyset_iter *k_iter)
{
	header_iter *p_data = (header_iter *)k_iter->private_data;
	keyset res;
	res.ppa = -1;

	if (p_data != NULL)
	{
		if (GETNUMKEY(p_data->header_data) > p_data->idx)
		{
			uint16_t *bitmap = GETBITMAP(p_data->header_data);
			char *data = p_data->header_data;
			int idx = p_data->idx;
			res.ppa = *((ppa_t *)&data[bitmap[idx]]);
			res.lpa.key = ((char *)&(data[bitmap[idx] + sizeof(ppa_t)]));
			res.lpa.len = bitmap[idx + 1] - bitmap[idx] - sizeof(ppa_t);
			p_data->idx++;
		}
		else
		{
			free(p_data);
			k_iter->private_data = NULL;
		}
	}
	return res;
}

void array_header_next_key_pick(level *lev, keyset_iter *k_iter, keyset *res)
{
	header_iter *p_data = (header_iter *)k_iter->private_data;
	if (GETNUMKEY(p_data->header_data) > p_data->idx)
	{
		uint16_t *bitmap = GETBITMAP(p_data->header_data);
		char *data = p_data->header_data;
		int idx = p_data->idx;
		res->ppa = *((ppa_t *)&data[bitmap[idx]]);
		res->lpa.key = ((char *)&(data[bitmap[idx] + sizeof(ppa_t)]));
		res->lpa.len = bitmap[idx + 1] - bitmap[idx] - sizeof(ppa_t);
	}
	else
	{
		res->ppa = -1;
	}
}

void array_normal_merger(skiplist *skip, run_t *r, bool iswP)
{
	ppa_t *ppa_ptr;
	KEYT key;
	char *body;
	int idx;
	body = data_from_run(r);
	uint16_t *bitmap = (uint16_t *)body;
	for_each_header_start(idx, key, ppa_ptr, bitmap, body)
	{
		skiplist_insert_wP(skip, key, *ppa_ptr, *ppa_ptr == UINT32_MAX ? false : true);
	}
	for_each_header_end
}

void array_checking_each_key(char *data, void *(*test)(KEYT a, ppa_t pa))
{
	ppa_t *ppa_ptr;
	KEYT key;
	int idx;
	uint16_t *bitmap = (uint16_t *)data;
	for_each_header_start(idx, key, ppa_ptr, bitmap, data)
		test(key, *ppa_ptr);
	for_each_header_end
}

int array_cache_comp_formatting(level *lev, run_t ***des, bool des_cache)
{
	array_body *b = (array_body *)lev->level_data;
	run_t *arrs = b->arrs;
	// static int cnt=0;
	// can't caculate the exact nubmer of run...
	run_t **res = (run_t **)malloc(sizeof(run_t *) * (lev->n_num + 1));

	for (int i = 0; i < lev->n_num; i++)
	{
		if (des_cache)
		{
			res[i] = &arrs[i];
		}
		else
		{
			res[i] = array_make_run(arrs[i].key, arrs[i].end, arrs[i].pbn);
			// res[i]->cpt_data = ISNOCPY(LSM.setup_values) ? htable_assign(arrs[i].level_caching_data, 0) : htable_assign(arrs[i].level_caching_data, 1);
		}
	}
	res[lev->n_num] = NULL;
	*des = res;
	return lev->n_num;
}