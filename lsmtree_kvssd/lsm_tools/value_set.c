#include "value_set.h"
#include <stdlib.h>
#include <string.h>

int v_cnt[PIECE_PER_PAGE + 1];int v_cnt[PIECE_PER_PAGE + 1];

value_set *inf_get_valueset(PTR in_value, uint32_t length)
{
    // 1. 前置校验：length 不能为0/超大值（防止恶意/错误输入）
    if (length == 0 || length > (1024 * 1024 * 1024)) {  // 限制最大1GB，可调整
        fprintf(stderr, "Error: invalid length %u for value_set\n", length);
        return NULL;
    }

    // 2. 分配value_set结构体，检查malloc是否失败
    value_set *res = (value_set *)malloc(sizeof(value_set));
    if (res == NULL) {  // 关键：检查malloc返回值
        perror("malloc failed for value_set");  // 打印具体失败原因
        abort();  // 若必须终止，先打印日志；也可返回NULL让上层处理
    }

    // 3. 安全计算length（向上取整到PIECE的整数倍）
    uint32_t aligned_length = ((length + PIECE - 1) / PIECE) * PIECE;  // 简化等价写法，更易读
    res->length = aligned_length;

    // 4. 分配value内存，检查是否失败
    res->value = (PTR)malloc(aligned_length);
    if (res->value == NULL) {
        perror("malloc failed for value_set->value");
        free(res);  // 释放已分配的结构体，避免内存泄漏
        return NULL;
    }

    v_cnt[aligned_length / PIECE]++;

    // 5. 安全拷贝/初始化内存
    if (in_value) {
        // 仅拷贝原始length字节，而非对齐后的长度，避免越界读取in_value
        memcpy(res->value, in_value, length);
        // 剩余空间置0（可选，增强安全性）
        memset((char*)res->value + length, 0, aligned_length - length);
    } else {
        memset(res->value, 0, aligned_length);
    }

    return res;
}

void inf_free_valueset(value_set **in)
{
    // 修复：先检查in和*in是否为NULL，避免空指针解引用
    if (!in || !(*in)) {
        return;
    }

    // 先释放value，再释放结构体
    free((*in)->value);
    free(*in);
    *in = NULL;  // 置空，避免野指针
}