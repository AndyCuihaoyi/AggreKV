#include "blktrace.h"
#include <stdio.h>
#include <stdlib.h>


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <blktrace_file>\n", argv[0]);
        return EXIT_FAILURE;
    }
    blktrace_file_content *content = parse_blktrace_file(argv[1]);
    if (!content) {
        return EXIT_FAILURE;
    }
    printf("Parsed %lu records from blktrace file.\n", content->num_records);
    // 可以在这里对 content->records 进行进一步处理
    free(content->records);
    free(content);
    return EXIT_SUCCESS;
}