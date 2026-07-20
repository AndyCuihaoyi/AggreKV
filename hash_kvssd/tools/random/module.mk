RANDOM_SRCS = $(wildcard tools/random/*.c)
RANDOM_DEPS = $(RANDOM_SRCS:.c=.d)  # 对应的依赖文件名
RANDOM_OBJS = $(RANDOM_SRCS:.c=.o)

ALL_SRCS += $(RANDOM_SRCS)
ALL_DEPS += $(RANDOM_DEPS)
ALL_OBJS += $(RANDOM_OBJS)