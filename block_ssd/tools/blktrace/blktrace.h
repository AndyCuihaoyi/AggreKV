#ifndef __BLKTRACE_H__
#define __BLKTRACE_H__

#include <stdint.h>

enum iotype {
    IO_TYPE_READ = 0,
    IO_TYPE_WRITE = 1,
    IO_TYPE_TRIM = 2,
    IO_TYPE_MAX,
};

typedef struct tag_blktrace_record {
    enum iotype io_type;
    uint64_t offset_secs;
    uint64_t length_secs;
    double ts;
} blktrace_record;

typedef struct tag_blktrace_file_content {
    uint64_t num_records;
    blktrace_record *records;
    // private
    uint64_t _cap_num_records; // 用于动态扩容
} blktrace_file_content;

blktrace_file_content *blktrace_file_content_create(void);
int blktrace_file_content_append_record(blktrace_file_content *content,
                                        const blktrace_record *record);
void blktrace_file_content_destroy(blktrace_file_content *content);
blktrace_file_content *parse_blktrace_file(const char *filename);

#endif // __BLKTRACE_H__
