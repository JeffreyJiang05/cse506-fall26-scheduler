#ifndef DEBUG_BPF_H
#define DEBUG_BPF_H

#include <scx/common.bpf.h>

#ifdef VERBOSE
    #define DEBUG
#endif

#ifdef DEBUG 
#define bpf_debug(fmt, ...) do { \
    bpf_printk(fmt, ##__VA_ARGS__); \
} while (0)
#else
#define bpf_debug(fmt, ...)
#endif

#endif