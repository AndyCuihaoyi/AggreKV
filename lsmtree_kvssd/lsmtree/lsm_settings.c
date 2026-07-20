#include "lsm_settings.h"
#include "lsmtree.h"
#include <math.h>
#include <stdio.h>
extern lsmtree LSM;

void lsm_setup_params()
{
    LSM.lsp.VALUESIZE = 1024;
    LSM.lsp.total_memory = TOTALSIZE / 1024*10; // TOTALSIZE=300G，total_memory=300MB=300*1024*1024B
    LSM.lsp.remain_memory = LSM.lsp.total_memory;
    LSM.lsp.LEVELN = LSM.LEVELN;
    LSM.lsp.KEYNUM = MAXKEY_IN_METAPAGE;                                                                 // 在metadata page的最多key数量，页大小/32
    LSM.lsp.ONESEGMENT = LSM.lsp.KEYNUM * LSM.lsp.VALUESIZE;                                             // 一个metapage 对应的指的大小为数据段大小
    LSM.lsp.HEADERNUM = (TOTALSIZE / LSM.lsp.ONESEGMENT) + (TOTALSIZE % LSM.lsp.ONESEGMENT ? 1 : 0); // showing size分成segment大小，每个segment需要一个header
    uint32_t TOTALHEADER = (TOTALSIZE / LSM.lsp.ONESEGMENT) + (TOTALSIZE % LSM.lsp.ONESEGMENT ? 1 : 0);  // 全填满所需段的总数，包括OP
    // level list中的header有metapage的 start key和一个指向物理页的指针4B
    LSM.lsp.remain_memory -= (TOTALHEADER * (DEFKEYLENGTH + 4));
    // LSM.lsp.bf_fprs = (float *)calloc(sizeof(float), LSM.LEVELN); // 存储每一层的假阳率
    //  get_sizefactor set pinning memory, caculate remainmemory
    //  获取层大小因数，将top K层的内存空间减去
    LSM.llp.size_factor = get_sizefactor(LSM.lsp.KEYNUM);
    printf("level size factor: %f \n", LSM.llp.size_factor);
}

void lsm_volumn_print(float size_factor)
{

    float meta_volumn = 0;
    float data_volumn = 0;
    float meta_volumn_total = 0;
    float data_volumn_total = 0;
    int all_header_num = 0;
    float target = size_factor;
    for (int i = 0; i <= LSM.LEVELN; i++)
    {
        if (i == 0)
        {
            meta_volumn = PAGESIZE;
            data_volumn = (PAGESIZE / (DEFKEYLENGTH + POINTER_SIZE)) * (DEFVALUESIZE);
        }
        else
        {
            all_header_num += round(target);
            meta_volumn = target * PAGESIZE;
            data_volumn = target * (PAGESIZE / (DEFKEYLENGTH + POINTER_SIZE)) * (DEFVALUESIZE);
            target *= size_factor;
            // meta_volumn = size_factor * meta_volumn;
        }
        meta_volumn_total += meta_volumn;
        data_volumn_total += data_volumn;

        if (meta_volumn > G)
        {
            printf("level %d:meta is %f GB \n", i, meta_volumn / G);
        }
        else if (meta_volumn >= M)
        {
            printf("level %d:meta is %f MB \n", i, meta_volumn / M);
        }
        else if (meta_volumn >= K)
        {
            printf("level %d:meta is %f KB \n", i, meta_volumn / K);
        }
        if (data_volumn > G)
        {
            printf("level %d:data is %f GB \n", i, data_volumn / G);
        }
        else if (data_volumn >= M)
        {
            printf("level %d:data is %f MB \n", i, data_volumn / M);
        }
        else if (data_volumn >= K)
        {
            printf("level %d:data is %f KB \n", i, data_volumn / K);
        }
    }
    printf("data volumn:  %fGB ;meta volumn:  %fGB \n", data_volumn_total / G, meta_volumn_total / G);
    printf("all header num check: %d \n", all_header_num);
}

float get_sizefactor(uint32_t keynum_in_header)
{
    uint32_t _f = LSM.LEVELN;
    float res;

    LSM.lsp.ONESEGMENT = LSM.lsp.KEYNUM * LSM.lsp.VALUESIZE;
    // LSM.lsp.HEADERNUM=SHOWINGSIZE/LSM.lsp.ONESEGMENT+(SHOWINGSIZE%LSM.lsp.ONESEGMENT?1:0);
    res = _f ? ceil(pow(10, log10(LSM.lsp.HEADERNUM) / (_f))) : LSM.lsp.HEADERNUM / keynum_in_header;
    // 根据总层数不断调整size_factor使得逻辑segment数不会超过物理限制
    int i = 0;
    float ff = 0.05f;
    float cnt = 0;
    uint64_t all_header_num;
    uint32_t before_last_header = 0;
    float target;
retry:
    all_header_num = 0;
    target = res;
    for (i = 0; i < LSM.LEVELN; i++)
    {
        all_header_num += round(target);
        before_last_header = round(target);
        target *= res;
    }

    if (all_header_num > LSM.lsp.HEADERNUM)
    {
        res -= ff;
        goto retry;
    }
    target = res;
    res = res - (ff * (cnt ? cnt - 1 : 0));

    uint32_t sum = 0;
    while (1)
    {
        sum = 0; // 计算top K层总的节点数
        uint32_t ptr = 1;
        for (int i = 0; i < LSM.LEVELCACHING; i++)
        {
            ptr = ceil(res * ptr);
            sum += ptr;
        }
        if (sum * PAGESIZE < LSM.lsp.total_memory)
        {
            // 判断能否将top K层的metapage segment放在内存中
            break;
        }
        else
        {
            LSM.lsp.LEVELCACHING = LSM.LEVELCACHING;
            printf("change level pinning level to %d\n", LSM.LEVELCACHING);
        }
    }

    LSM.lsp.pin_memory = sum * PAGESIZE;
    LSM.lsp.remain_memory = LSM.lsp.remain_memory - LSM.lsp.pin_memory;
    LSM.llp.last_size_factor = res;
    printf("all header num: %ld\n", all_header_num);
    // lsm_volumn_print(res); // for debug
    return res;
}