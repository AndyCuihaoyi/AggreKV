HASH_HOT_CMT_SRCS = $(wildcard hash_hot_cmt/*.c)
HASH_HOT_CMT_DEPS = $(HASH_HOT_CMT_SRCS:.c=.d)  # 对应的依赖文件名
HASH_HOT_CMT_OBJS = $(HASH_HOT_CMT_SRCS:.c=.o)
HASH_HOT_CMT_INCLUDE = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/

ALL_SRCS += $(HASH_HOT_CMT_SRCS)
ALL_DEPS += $(HASH_HOT_CMT_DEPS)
ALL_OBJS += $(HASH_HOT_CMT_OBJS)

hash_hot_cmt/%.o: CFLAGS += $(HASH_HOT_CMT_INCLUDE)