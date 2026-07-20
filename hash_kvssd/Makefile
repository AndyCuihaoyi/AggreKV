TARGET = test_ext_mem_lat
# CFLAGS = -g -Wall -fcommon -lm -lglib-2.0 -DHOT_CMT -DDATA_SEGREGATION -DADAPTIVE_MEM
# ALL_TARGETS = hash_hot_cmt/test_ext_mem_lat_asan hash_hot_cmt/test_ext_mem_lat
# CONFIG_FILES = Makefile $(wildcard */module.mk)
CFLAGS = -g -Wall -fcommon -lm -lglib-2.0
ALL_TARGETS = hash_hot_cmt/test_ext_mem_lat_asan hash_hot_cmt/test_ext_mem_lat
CONFIG_FILES = Makefile $(wildcard */module.mk)
ifeq ($(TARGET), test_ext_mem_lat_asan)
	CFLAGS += -fsanitize=address -fno-omit-frame-pointer
endif

ALL_SRCS =
ALL_DEPS =
ALL_OBJS =

include tools/module.mk
include lower/module.mk
include hash_hot_cmt/module.mk

.PHONY: all
all: $(TARGET)

test_ext_mem_lat: $(ALL_OBJS)
	$(CC) $^ $(CFLAGS) -o hash_hot_cmt/$@

test_ext_mem_lat_asan: $(ALL_OBJS)
	$(CC) $^ $(CFLAGS) -o hash_hot_cmt/$@

%.o: %.c $(CONFIG_FILES)
	$(CC) -MMD -MP -c -o $@ $< $(CFLAGS)

-include $(ALL_DEPS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(ALL_TARGETS)