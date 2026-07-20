TOOLS_SRCS = $(wildcard tools/*.c)
TOOLS_DEPS = $(TOOLS_SRCS:.c=.d)  # 对应的依赖文件名
TOOLS_OBJS = $(TOOLS_SRCS:.c=.o)

include tools/random/module.mk
include tools/rte_ring/module.mk
include tools/blktrace/module.mk

ALL_SRCS += $(TOOLS_SRCS)
ALL_DEPS += $(TOOLS_DEPS)
ALL_OBJS += $(TOOLS_OBJS)