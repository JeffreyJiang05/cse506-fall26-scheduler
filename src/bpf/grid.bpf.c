/* SPDX-License-Identifier: GPL-2.0 */
/* Grid scheduler
 * 
 */

#include <scx/common.bpf.h>

#include "util.bpf.h"
#include "config.h"

char _license[] SEC("license") = "GPL";
UEI_DEFINE(uei);

/** 
 * Compute DSQ ID based on the CPU, row, and column 
 */
#define GRID_DSQ_ID(cpu, row, col) (((cpu) << (ROW_BITS + COL_BITS)) | ((row) << COL_BITS) | (col))

// Calculate row, column, and CPU a DSQ belongs to based on its ID
#define GRID_DSQ_COL(id) ((id) & MASK(COL_BITS))
#define GRID_DSQ_ROW(id) (((id) >> COL_BITS) & MASK(ROW_BITS))
#define GRID_DSQ_CPU(id) ((id) >> (ROW_BITS + COL_BITS))

/**
 * Data to be populated by userspace
 */
const volatile u32 nr_cpus; // Number of CPU cores
const volatile u16 latency[GRID_NR_ROWS]; // Latency for each row in grid
const volatile u16 slices[GRID_NR_COLS]; // Time slices for each column in grid


s32 BPF_STRUCT_OPS_SLEEPABLE(grid_init)
{

	// Hard-coded here
    bpf_printk("Initializing Grid Scheduler");
	bpf_printk("Total number of cores: %u", nr_cpus);
	bpf_printk("Latencies: [%u %u %u %u %u %u %u %u]", 
		latency[0], latency[1], latency[2], latency[3],
		latency[4], latency[5], latency[6], latency[7]);
	bpf_printk("Time slices: [%u %u %u %u %u %u %u %u]", 
		slices[0], slices[1], slices[2], slices[3],
		slices[4], slices[5], slices[6], slices[7]);

	return 0;
}

void BPF_STRUCT_OPS(grid_exit, struct scx_exit_info *ei)
{
	bpf_printk("Terminating Grid Scheduler");
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(grid_ops,
	       .init			= (void*) grid_init,
	       .exit			= (void*) grid_exit,
	       .name			= "grid");
