CC = clang
TARGET = sched

SRCD = src
BLDD = build
BIND = bin
INCD = include

SRCS = $(SRCD)/sched.c
OBJS = $(SRCS:$(SRCD)/%.c=$(BLDD)/%.o)

BPFS  = $(shell find $(SRCD)/bpf -name "*.bpf.c")
OBPFS = $(BPFS:$(SRCD)/bpf/%.bpf.c=$(BLDD)/bpf/%.bpf.o)
SKELE = $(OBPFS:$(BLDD)/bpf/%.bpf.o=$(BLDD)/include/%.skel.h)

INCS = -I$(INCD) -I$(BLDD)/include
LIBS = -lbpf
CFLAGS = -Wall -Wextra -g

.PRECIOUS: $(BLDD)/bpf/%.bpf.o $(BLDD)/include/%.skel.h

$(BIND)/$(TARGET): $(OBJS) | $(BIND)
	$(CC) $(CFLAGS) $(INCS) -o $@ $(OBJS) $(LIBS)

$(BLDD)/%.o: $(SRCD)/%.c $(SKELE) | $(BLDD)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@ 

$(BLDD)/include/vmlinux.h: | $(BLDD)/include
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

$(BLDD)/bpf/%.bpf.o: $(SRCD)/bpf/%.bpf.c $(BLDD)/include/vmlinux.h | $(BLDD)/bpf
	$(CC) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I$(BLDD)/include -c $< -o $@

$(BLDD)/include/%.skel.h: $(BLDD)/bpf/%.bpf.o | $(BLDD)/include
	bpftool gen skeleton $< > $@

$(BLDD)/include: | $(BLDD)
	mkdir $(BLDD)/include

$(BLDD)/bpf: | $(BLDD)
	mkdir $(BLDD)/bpf

$(BLDD):
	mkdir $(BLDD)

$(BIND):
	mkdir $(BIND)

clean:
	rm -rfv $(BLDD)
	rm -rfv $(BIND)

.phony: clean