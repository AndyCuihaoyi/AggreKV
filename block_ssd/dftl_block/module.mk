DFTL_BLOCK_SRCS = \
	dftl_block/algo_queue.c \
	dftl_block/demand.c \
	dftl_block/dftl_bm.c \
	dftl_block/dftl_cache.c \
	dftl_block/dftl_pg.c \
	dftl_block/dftl_utils.c \
	dftl_block/dftl_wb.c \
	dftl_block/request.c

DFTL_BLOCK_DEPS = $(DFTL_BLOCK_SRCS:.c=.d)
DFTL_BLOCK_OBJS = $(DFTL_BLOCK_SRCS:.c=.o)
DFTL_BLOCK_INCLUDE = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/

ALL_SRCS += $(DFTL_BLOCK_SRCS)
ALL_DEPS += $(DFTL_BLOCK_DEPS)
ALL_OBJS += $(DFTL_BLOCK_OBJS)

dftl_block/%.o: CFLAGS += $(DFTL_BLOCK_INCLUDE)