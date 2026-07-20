TARGET = test_blktrace
CFLAGS = -g -Wall -fcommon -lm -lglib-2.0 -DBLOCK_SSD
ALL_TARGETS = \
	dftl_block/test_ext_mem_lat \
	dftl_block/test_ext_mem_lat_asan \
	dftl_block/test_blktrace \
	dftl_block/test_blktrace_asan \
	tools/blktrace/ut_blktrace

CONFIG_FILES = Makefile $(wildcard */module.mk)

ifeq ($(TARGET), test_ext_mem_lat_asan)
	CFLAGS += -O0 -fsanitize=address -fno-omit-frame-pointer
endif
ifeq ($(TARGET), test_blktrace_asan)
	CFLAGS += -O0 -fsanitize=address -fno-omit-frame-pointer
endif

ALL_SRCS =
ALL_DEPS =
ALL_OBJS =

include tools/module.mk
include lower/module.mk
include dftl_block/module.mk

.PHONY: all test_ext_mem_lat test_ext_mem_lat_asan test_blktrace test_blktrace_asan ut_blktrace
all: $(TARGET)

test_ext_mem_lat: dftl_block/test_ext_mem_lat
test_ext_mem_lat_asan: dftl_block/test_ext_mem_lat_asan
test_blktrace: dftl_block/test_blktrace
test_blktrace_asan: dftl_block/test_blktrace_asan
ut_blktrace: tools/blktrace/ut_blktrace

dftl_block/test_ext_mem_lat: $(ALL_OBJS) dftl_block/test_ext_mem_lat.o
	$(CC) $^ $(CFLAGS) -o $@

dftl_block/test_ext_mem_lat_asan: $(ALL_OBJS) dftl_block/test_ext_mem_lat.o
	$(CC) $^ $(CFLAGS) -o $@

dftl_block/test_blktrace: $(ALL_OBJS) dftl_block/test_blktrace.o
	$(CC) $^ $(CFLAGS) -o $@

dftl_block/test_blktrace_asan: $(ALL_OBJS) dftl_block/test_blktrace.o
	$(CC) $^ $(CFLAGS) -o $@

tools/blktrace/ut_blktrace: tools/blktrace/blktrace.o tools/blktrace/ut_blktrace.o
	$(CC) $^ $(CFLAGS) -o $@

%.o: %.c $(CONFIG_FILES)
	$(CC) -MMD -MP -c -o $@ $< $(CFLAGS)

-include $(ALL_DEPS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(ALL_TARGETS) \
		$(addsuffix .o, $(ALL_TARGETS)) \
		$(addsuffix .d, $(ALL_TARGETS))
