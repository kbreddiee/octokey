# Host-side unit tests for the portable espkvm components.
# Run: make -C tests/host test

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11
COMMON   = ../../common/components
INCLUDES = -I$(COMMON)/kvm_proto/include -I$(COMMON)/kvm_hidparse/include

OUT = out

all: $(OUT)/test_proto $(OUT)/test_hidparse

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_proto: test_proto.c $(COMMON)/kvm_proto/kvm_proto.c | $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(OUT)/test_hidparse: test_hidparse.c $(COMMON)/kvm_hidparse/kvm_hidparse.c | $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test: all
	$(OUT)/test_proto
	$(OUT)/test_hidparse

clean:
	rm -rf $(OUT)

.PHONY: all test clean
