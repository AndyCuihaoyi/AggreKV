#include "lsm_utils.h"

uint64_t timer_start_ns;
char *kvssd_tostring(KEYT key)
{
    /*
    char temp1[255]={0,};
    memcpy(temp1,key.key,key.len);*/
    return key.key;
}

void kvssd_cpy_key(KEYT *des, KEYT *key)
{
    des->key = (char *)malloc(key->len);
    des->len = key->len;
    memcpy(des->key, key->key, key->len);
}
void kvssd_free_key(KEYT *des)
{
    free(des->key);
    free(des);
}

void htable_print(char *target)
{
    uint16_t *bitmap = (uint16_t *)target;
    uint16_t data_start;
    int key_cnt = bitmap[0];
    uint32_t idx;
    for (idx = 1; idx < key_cnt; idx++)
    {
        data_start = bitmap[idx];
        int key_length = bitmap[idx + 1] - bitmap[idx] - sizeof(ppa_t);
        ppa_t temp_ppa;
        memcpy(&temp_ppa, &target[data_start], sizeof(ppa_t));
        KEYT temp_key;
        temp_key.key = malloc(sizeof(char) * (key_length + 1));
        temp_key.key[key_length] = '\0';
        memcpy(temp_key.key, &target[data_start + sizeof(ppa_t)], key_length);
        printf("idx: %d ; ppa: %d ; key: %s\n", idx, temp_ppa, temp_key.key);
        free(temp_key.key);
    }
}

bool keyset_check(char *target)
{
    uint16_t *bitmap = (uint16_t *)target;
    uint16_t data_start;
    int key_cnt = bitmap[0];
    uint32_t idx;
    for (idx = 1; idx < key_cnt; idx++)
    {
        data_start = bitmap[idx];
        int key_length = bitmap[idx + 1] - bitmap[idx] - sizeof(ppa_t);
        ppa_t temp_ppa;
        memcpy(&temp_ppa, &target[data_start], sizeof(ppa_t));
        KEYT temp_key;
        temp_key.key = malloc(sizeof(char) * (key_length + 1));
        temp_key.key[key_length] = '\0';
        temp_key.len = key_length;
        memcpy(temp_key.key, &target[data_start + sizeof(ppa_t)], key_length);
        if (!KEYVALCHECK(temp_key))
        {
            free(temp_key.key);
            return false;
        }
        free(temp_key.key);
    }
    return true;
}