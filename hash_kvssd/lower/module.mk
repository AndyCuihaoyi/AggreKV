LOWER_SRCS = $(wildcard lower/*.c)
LOWER_DEPS = $(LOWER_SRCS:.c=.d)  # 对应的依赖文件名
LOWER_OBJS = $(LOWER_SRCS:.c=.o)
LOWER_INCLUDE = -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include/

ALL_SRCS += $(LOWER_SRCS)
ALL_DEPS += $(LOWER_DEPS)
ALL_OBJS += $(LOWER_OBJS)

lower/%.o: CFLAGS += $(LOWER_INCLUDE)