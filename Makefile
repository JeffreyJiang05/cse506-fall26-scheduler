CC = clang
TARGET = grid_sched

# Directories
SRCD = src
BLDD = build
BIND = bin
INCD = include

# Source C files
SRCS = $(SRCD)/sched.c
OBJS = $(SRCS:$(SRCD)/%.c=$(BLDD)/%.o)

# Source BPF files
BPFS  = $(shell find $(SRCD)/bpf -name "*.bpf.c")
OBPFS = $(BPFS:$(SRCD)/bpf/%.bpf.c=$(BLDD)/bpf/%.bpf.o)
SKELE = $(OBPFS:$(BLDD)/bpf/%.bpf.o=$(BLDD)/include/%.skel.h)

# SCX Version
SCX_VERSION = 1.0.17
SCX_URL = https://github.com/sched-ext/scx/archive/refs/tags/v$(SCX_VERSION).tar.gz
SCX_VERSION_STAMP = $(BLDD)/scx.version

# libBPF source
LIBBPF_SRC = libbpf/src
LIBBPF_A = $(BLDD)/lib/libbpf.a

# Flags
INCS = -I$(INCD) -I$(BLDD)/include
LIBS = -lelf -lz
CFLAGS = -Wall -g -Wno-unused-parameter
DFLAGS = -g -DVERBOSE -DCOLOR
BPF_FLAGS = 

.PRECIOUS: $(BLDD)/bpf/%.bpf.o $(BLDD)/include/%.skel.h $(BLDD)/include/scx/*.h

# BUILD VERSION STAMP
BUILD_VERSION_STAMP = $(BLDD)/version

prod: BUILD_VERSION := prod
prod: $(BIND)/$(TARGET)

debug: BUILD_VERSION := debug
debug: CFLAGS += $(DFLAGS)
debug: BPF_FLAGS += $(DFLAGS)
debug: $(BIND)/$(TARGET)

.PHONY: FORCE
$(BUILD_VERSION_STAMP): FORCE | $(BLDD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(BUILD_VERSION)" ]; then \
		echo "$(BUILD_VERSION)" > $@; \
	fi

# eBPF Scheduler Launcher
$(BIND)/$(TARGET): $(OBJS) $(LIBBPF_A) | $(BIND)
	$(CC) $(CFLAGS) $(INCS) -o $@ $(OBJS) $(LIBBPF_A) $(LIBS)

# Build source files
$(BLDD)/%.o: $(SRCD)/%.c $(BUILD_VERSION_STAMP) $(SKELE) $(LIBBPF_A) $(BLDD)/include/scx | $(BLDD)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@ 

# Build BPF files
$(BLDD)/bpf/%.bpf.o: $(SRCD)/bpf/%.bpf.c $(BLDD)/include/vmlinux.h | $(BLDD)/bpf
	$(CC) -g -O2 -target bpf -mcpu=v3 -D__TARGET_ARCH_x86 $(BPF_FLAGS) $(INCS) -c $< -o $@

# Create $(BLDD)/include/scx and unpack scx includes
$(BLDD)/include/scx: $(BLDD)/scx.tar.gz | $(BLDD)/include
	@mkdir $@
	@tar -xzf $< --strip-components=4 -C $@ scx-$(shell cat $(SCX_VERSION_STAMP))/scheds/include/scx
	@tar -xzf $< --strip-components=3 -C $(BLDD)/include --wildcards 'scx-$(shell cat $(SCX_VERSION_STAMP))/scheds/include/*.h'

# Version stamp the fetch SCX
$(SCX_VERSION_STAMP): FORCE | $(BLDD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(SCX_VERSION)" ]; then \
		echo "$(SCX_VERSION)" > $@; \
	fi

# Fetch scx.tar.gz
$(BLDD)/scx.tar.gz: $(SCX_VERSION_STAMP) | $(BLDD)
	@rm -f $@
	@rm -rf $(BLDD)/include/scx
	@curl -sSL $(SCX_URL) -o $@

# Create BTF vmlinux.h for libBPF CO-RE
$(BLDD)/include/vmlinux.h: | $(BLDD)/include
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# Build libBPF skeleton header files
$(BLDD)/include/%.skel.h: $(BLDD)/bpf/%.bpf.o | $(BLDD)/include
	bpftool gen skeleton $< > $@

# Build libbpf
$(LIBBPF_A): | $(BLDD)/lib
	$(MAKE)	-C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1
	@mv $(LIBBPF_SRC)/libbpf.a $@

# Create directories in build
$(BLDD)/lib $(BLDD)/bpf: | $(BLDD)
	@mkdir $@

$(BLDD)/include: | $(BLDD)
	@mkdir $@
	@ln -sfn $(abspath $(LIBBPF_SRC)) $(BLDD)/include/bpf

# Create output directories
$(BLDD) $(BIND):
	@mkdir $@

# Clean 
clean:
	@rm -rfv $(BLDD)/include
	@rm -rfv $(BLDD)/bpf
	@rm -rfv $(BIND)

.phony: clean