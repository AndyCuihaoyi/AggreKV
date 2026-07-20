#include "blktrace.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define BLKTRACE_FILE_CONTENT_REALLOC_G 32768 // 每次扩容的记录数

blktrace_file_content *blktrace_file_content_create(void) {
    blktrace_file_content *content =
        (blktrace_file_content *)malloc(sizeof(blktrace_file_content));
    if (!content) {
        perror("Failed to allocate memory for blktrace content");
        return NULL;
    }

    content->num_records = 0;
    content->_cap_num_records = BLKTRACE_FILE_CONTENT_REALLOC_G;
    content->records = (blktrace_record *)malloc(sizeof(blktrace_record) *
                                                 content->_cap_num_records);
    if (!content->records) {
        perror("Failed to allocate memory for blktrace records");
        free(content);
        return NULL;
    }

    return content;
}

int blktrace_file_content_append_record(blktrace_file_content *content,
                                        const blktrace_record *record) {
    if (!content || !record) {
        return -1;
    }

    if (content->num_records >= content->_cap_num_records) {
        uint64_t new_capacity =
            content->_cap_num_records + BLKTRACE_FILE_CONTENT_REALLOC_G;
        blktrace_record *new_records =
            (blktrace_record *)realloc(content->records,
                                       sizeof(blktrace_record) * new_capacity);
        if (!new_records) {
            perror("Failed to reallocate memory for blktrace records");
            return -1;
        }
        content->records = new_records;
        content->_cap_num_records = new_capacity;
    }

    content->records[content->num_records++] = *record;
    return 0;
}

void blktrace_file_content_destroy(blktrace_file_content *content) {
    if (!content) {
        return;
    }

    free(content->records);
    free(content);
}


// blktrace format: [io_type(0,1)],[offset_secs],[length_secs],[ts]
blktrace_file_content *parse_blktrace_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open blktrace file");
        return NULL;
    }
    blktrace_file_content *content = blktrace_file_content_create();
    if (!content) {
        fclose(file);
        return NULL;
    }

    // 读取文件内容
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        int io_type;
        blktrace_record record;
        if (sscanf(line, "%d,%" SCNu64 ",%" SCNu64 ",%lf", &io_type,
                   &record.offset_secs, &record.length_secs, &record.ts) != 4) {
            fprintf(stderr, "Invalid line format: %s", line);
            continue;
        }
        record.io_type = (enum iotype)io_type;
        if (blktrace_file_content_append_record(content, &record) != 0) {
            blktrace_file_content_destroy(content);
            fclose(file);
            return NULL;
        }
    }
    fclose(file);
    return content;
}

