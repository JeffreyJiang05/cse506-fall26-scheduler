/* SPDX-License-Identifier: GPL-2.0 */
/* Grid scheduler
 * 
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

s32 BPF_STRUCT_OPS_SLEEPABLE(grid_init)
{
    bpf_printk("Initializing Grid Scheduler");
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
