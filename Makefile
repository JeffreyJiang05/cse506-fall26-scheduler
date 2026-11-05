CC = clang
TARGET = sched

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
SCX_VERSION = 1.1.4
SCX_URL = https://github.com/sched-ext/scx/archive/refs/tags/v$(SCX_VERSION).tar.gz
SCX_VERSION_STAMP = $(BLDD)/scx.version

# Flags
INCS = -I$(INCD) -I$(BLDD)/include
LIBS = -lbpf
CFLAGS = -Wall -Wextra -g

.PRECIOUS: $(BLDD)/bpf/%.bpf.o $(BLDD)/include/%.skel.h $(BLDD)/include/scx/*.h

# eBPF Scheduler Launcher
$(BIND)/$(TARGET): $(OBJS) $(BLDD)/include/scx | $(BIND)
	$(CC) $(CFLAGS) $(INCS) -o $@ $(OBJS) $(LIBS)

# Build source files
$(BLDD)/%.o: $(SRCD)/%.c $(SKELE) | $(BLDD)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@ 

# Build BPF files
$(BLDD)/bpf/%.bpf.o: $(SRCD)/bpf/%.bpf.c $(BLDD)/include/vmlinux.h | $(BLDD)/bpf
	$(CC) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I$(BLDD)/include -c $< -o $@

# Create $(BLDD)/include/scx and unpack scx includes
$(BLDD)/include/scx: $(BLDD)/scx.tar.gz | $(BLDD)/include
	@mkdir $@
	@tar -xzf $< --strip-components=4 -C $@ scx-$(shell cat $(SCX_VERSION_STAMP) 2> /dev/null)/scheds/include/scx

# Version stamp the fetch SCX
.PHONY: FORCE
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

# Create directories in build
$(BLDD)/include $(BLDD)/bpf: | $(BLDD)
	@mkdir $@

# Create output directories
$(BLDD) $(BIND):
	@mkdir $@

# Clean 
clean:
	@rm -rfv $(BLDD)
	@rm -rfv $(BIND)

.phony: clean