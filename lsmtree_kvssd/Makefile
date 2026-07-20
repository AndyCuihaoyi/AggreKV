CC = gcc
CFLAGS = -fcommon -g -Wall -O2
INCLUDE = -I/usr/include/glib-2.0 \
          -I/usr/include/glib-2.0/glib \
          -I/usr/lib/x86_64-linux-gnu/glib-2.0/include
LIBS = -lm -lglib-2.0

# 源文件列表（来自 tasks.json）
SRCS = lsmtree/lsm_test.c \
       lsmtree/lsm_cache.c \
       lsmtree/lsm_utils.c \
       lsmtree/lsmtree.c \
       lsmtree/lsm_settings.c \
       lsmtree/compaction.c \
       lsmtree/compaction_assistant.c \
       lsmtree/lsm_bm.c \
       lsmtree/lsm_pm.c \
       lsm_tools/skiplist.c \
       lsm_tools/value_set.c \
       lsm_tools/queue.c \
       lsm_tools/latency_manager.c \
       lsm_tools/rte_ring/rte_ring.c \
       lower/ssd.c \
       array/array.c \
       array/array_level.c \
       array/pipe_merger.c \
       array/pipe.c

OBJS = $(SRCS:.c=.o)
TARGET = lsm_test

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(INCLUDE) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ $(INCLUDE)

clean:
	rm -f $(OBJS) $(TARGET)
