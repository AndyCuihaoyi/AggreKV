RTE_RING_SRCS += $(wildcard tools/rte_ring/*.c)
RTE_RING_DEPS = $(RTE_RING_SRCS:.c=.d)  # 对应的依赖文件名
RTE_RING_OBJS = $(RTE_RING_SRCS:.c=.o)

ALL_SRCS += $(RTE_RING_SRCS)
ALL_DEPS += $(RTE_RING_DEPS)
ALL_OBJS += $(RTE_RING_OBJS)