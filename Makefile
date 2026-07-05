UNAME_S := $(shell uname -s)
CC ?= cc
OUT := build

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -fPIC
LDLIBS_COMMON := -lpthread

ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT := dylib
  SHLIB_FLAGS := -dynamiclib -undefined dynamic_lookup
else
  SHLIB_EXT := so
  SHLIB_FLAGS := -shared
  LDLIBS_COMMON += -ldl
endif

INJECT_LIB := $(OUT)/libnccl_cq_fault_inject.$(SHLIB_EXT)

.PHONY: all clean

all: $(INJECT_LIB)

$(OUT):
	mkdir -p $(OUT)

$(INJECT_LIB): cq_fault_inject.c | $(OUT)
	$(CC) $(CFLAGS) $(SHLIB_FLAGS) $< -o $@ $(LDLIBS_COMMON)

clean:
	rm -rf $(OUT)
