/* Grid scheduler
 * 
 */

#include <scx/common.bpf.h>

#include "types.h"
#include "stats.bpf.h"

#include "debug.bpf.h"

char _license[] SEC("license") = "GPL";
UEI_DEFINE(uei);

// *========================== Useful Macros ==========================* //

#define DRR_MAX_ITERATIONS 3

/**
 * Assigns the task to the local CPU. Updates the task_metadata->dsq_id
 * to include the current local processor ID.
 */
#define task_metadata_assign_to_local_cpu(task_metadata) do { \
    u32 __cpu = bpf_get_smp_processor_id(); \
    (task_metadata)->dsq_id = GRID_DSQ_ID(__cpu, (task_metadata)->dsq_id); \
} while(0) 

// *========================== Userspace Input ==========================* //

/**
 * Data to be populated by userspace
 */
const volatile u32 nr_cpus; // Number of CPU cores
const volatile u16 latency[GRID_NR_ROWS]; // Latency for each row in grid
const volatile u16 slices[GRID_NR_COLS]; // Time slices for each column in grid
const volatile u16 drr_quanta[GRID_NR_ROWS]; // Quanta for DRR

static void debug_printk_config() {
    bpf_debug("nr_cpus = %d", nr_cpus);
    for (u32 i = 0; i < GRID_NR_ROWS; ++i) {
        bpf_debug("latency[%u] = %u", i, latency[i]);
    }
    for (u32 i = 0; i < GRID_NR_COLS; ++i) {
        bpf_debug("slices[%u] = %u", i, slices[i]);
    }
    for (u32 i = 0; i < GRID_NR_ROWS; ++i) {
        bpf_debug("drr_quanta[%u] = %u", i, drr_quanta[i]);
    }
}

#define for_each_cpu(cpu) for (u32 cpu = 0; cpu < nr_cpus; ++cpu) 

// *========================== eBPF Maps ==========================* //

// per-Task data used in scheduling
struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
    __uint(map_flags, BPF_F_NO_PREALLOC);
    __type(key, int);
    __type(value, struct task_metadata);
} grid_task_data SEC(".maps");

static inline struct task_metadata *get_task_metadata(struct task_struct *p) {
	return bpf_task_storage_get(&grid_task_data, p, &task_metadata_init, BPF_LOCAL_STORAGE_GET_F_CREATE);
}

// per-CPU data used in scheduling
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, u32);
    __type(value, struct cpu_state);
    __uint(max_entries, 1);
} grid_cpu_state SEC(".maps");

static inline struct cpu_state *get_cpu_state() {
	u32 key = 0;
	return bpf_map_lookup_elem(&grid_cpu_state, &key);
}

// *========================== SCX Callbacks ==========================* //

s32 BPF_STRUCT_OPS_SLEEPABLE(grid_init) {
    debug_printk_config();

    // Set up initial per-task data for scheduler
	task_metadata_initialize();

    u32 total_dsq_created = 0;
    bpf_debug("Initializing DSQs");
    for_each_cpu(cpu) {
        for_each_cpu_dsq(id, cpu) {
            if (scx_bpf_create_dsq(id, 0) < 0) {
                bpf_printk("Failed to create DSQ #%llu (CPU = %llu, row = %llu, col = %llu)", 
					id, GRID_DSQ_CPU(id), GRID_DSQ_ROW(id), GRID_DSQ_COL(id));
				return -1;
            }
            bpf_printk("Created DSQ #%llx", id);
            ++total_dsq_created;
        }
    }

    bpf_printk("CREATED TOTAL OF %u DSQs WITH %u CPUS", total_dsq_created, nr_cpus);
    bpf_debug("Initialized all DSQs successfully");

    return 0;
}


void BPF_STRUCT_OPS(grid_exit, struct scx_exit_info *ei) {
    bpf_debug("Destroying DSQs");
    for_each_cpu(cpu) {
        for_each_cpu_dsq(id, cpu) {
            scx_bpf_destroy_dsq(id);
        }
    }
    bpf_debug("Done destroying all DSQs");

    UEI_RECORD(uei, ei);
}

/**
 * Triggers when a task is being enqueued. Two possible cases:
 * 
 * (1) The task is new to the scheduler and its per-task data is not
 *     initialized. Set to default row and column positions
 * (2) The task has been scheduled before.
 * 
 * CPU is not assigned at the beginning of this callback. CPU gets assigned
 * based on the current processor executing this callback.
 */
void BPF_STRUCT_OPS(grid_enqueue, struct task_struct *p, u64 enq_flags) {
    u32 cpu = bpf_get_smp_processor_id();

    struct task_metadata *task_metadata = get_task_metadata(p);
    if (!task_metadata) {
        // Failed to allocate task metadata!
        scx_bpf_error("Failed to allocate task metadata");
        return;
    }

    // If the task's metadata has not been scheduled yet, set to default
    if (!is_task_metadata_initialized(task_metadata)) {
		task_metadata->dsq_id = GRID_DSQ_COORD(DEFAULT_ROW, DEFAULT_COL);
	}

    // All task_metadata->dsq_id stores the coordinate, no CPU info
    struct cpu_state *state = get_cpu_state();

    // Update CPU summary information
    cpu_state_enq_task(state, task_metadata->dsq_id);

    // Assign CPU to current @cpu
    task_metadata_assign_to_local_cpu(task_metadata);

    // Determine the timeslice allocated to task
    u32 col = GRID_DSQ_COL(task_metadata->dsq_id);
    task_metadata->timeslice_duration = slices[col] * NS;

    // Update enqueue time
    task_metadata->enqueue_time = bpf_ktime_get_ns();

    // Set flag that it is on the queue
    task_metadata->on_queue = true;
    
    // Insert into DSQ
    scx_bpf_dsq_insert(p, 
        task_metadata->dsq_id, 
        task_metadata->timeslice_duration, 
        0);

    bpf_debug("CPU #%u enqueued task %d (%s) on DSQ ID %llx", cpu, p->pid, p->comm, task_metadata->dsq_id);
    bpf_debug("Set summary flag to %llx", state->summary);
}

void BPF_STRUCT_OPS(grid_dequeue, struct task_struct *p, u64 deq_flags) {
    // To capture spurious parameter change or process being terminated
    // DOES NOT CALL WHEN A TASK IS MOVED FROM A USER DSQ TO A TERMINAL DSQ

    struct task_metadata *task_metadata = get_task_metadata(p);

	// If task is not initialized, then do nothing. Should not happen!
	if (!task_metadata || !is_task_metadata_initialized(task_metadata)) {
		return;
	}

	// If task is not on the queue, then do nothing
	if (!task_metadata->on_queue) {
		return;
	}

    // Mark task is not being on a queue anymore
    task_metadata->on_queue = false;

    // Reset fields of the task metadata
    task_metadata->enqueue_time = 0;
    task_metadata->timeslice_start = 0;
    task_metadata->timeslice_duration = 0;

    // Update the CPU state as needed
    struct cpu_state *state = get_cpu_state();
    cpu_state_deq_task(state, task_metadata->dsq_id);
}

// DRR specific operations

#define drr_can_schedule(cpu_state, row, cost) (cpu_state_row_metadata((cpu_state), (row)).deficit >= (cost))

#define drr_apply_quantum(cpu_state, row, quantum) do { \
	(cpu_state)->rows[(row)].deficit += (quantum); \
} while (0)

#define drr_apply_cost(cpu_state, row, cost) do { \
    (cpu_state)->rows[(row)].deficit -= (cost); \
} while (0)

void BPF_STRUCT_OPS(grid_dispatch, s32 cpu, struct task_struct *prev) {
    struct cpu_state *state = get_cpu_state();

    // No tasks in the grid, nothing to schedule
    if (!state->summary) {
        return;
    }

    bpf_debug("CPU #%d Summary %llx", cpu, state->summary);

    for (u32 iter = 0; iter < DRR_MAX_ITERATIONS; ++iter) {
        for_each_grid_row(row) {
            grid_summary_t row_summary = state->summary & grid_row_masks[row];

            // No task in the row, set the deficit to zero and move to next row
            if (!row_summary) {
                cpu_state_row_metadata(state, row).deficit = 0;
                continue;
            }

            // Row is not empty, increase the deficit by the quantum
            drr_apply_quantum(state, row, drr_quanta[row]);

            // Perform round robin within the row itself
            u32 start_col = cpu_state_row_metadata(state, row).next_col_sched;
            // Adjust row summary bitmap for RR
            row_summary >>= row * GRID_NR_ROWS;
            grid_summary_t rr_bitmask = (((row_summary << GRID_NR_COLS) | row_summary) >> start_col) & MASK(GRID_NR_COLS);

            for_each_grid_col(i) {
                // if rr_bitmask is empty, no more tasks to try to schedule
                if (!rr_bitmask) 
                    break;

                if (rr_bitmask & 0x1) {
                    u32 col = (start_col + i) % GRID_NR_COLS;
                    u32 cost = slices[col];

                    bpf_debug("Found a bit at row %u and col %u.", row, col);

                    // Check if the row can schedule a task with this cost
                    if (drr_can_schedule(state, row, cost)) {
                        // Schedule the task by moving it into the local DSQ
                        dsq_id_t dsq = GRID_DSQ_ID(cpu, GRID_DSQ_COORD(row, col));
                        bpf_debug("CPU #%d: Attempting to move task from DSQ %llx", cpu, dsq);
                        if (!scx_bpf_dsq_move_to_local(dsq)) {
                            // bpf_debug("Failed to move task from user DSQ %d to local DSQ on cpu %u."
                            //           "Likely caused summary bitmap corruption!", 
                            //           dsq, bpf_get_smp_processor_id());
                            // Move onto then next task
                            rr_bitmask >>= 1;
                            continue;
                        }

                        bpf_debug("Successfully scheduled a task");

                        // Successfully scheduled the task
                        // Decrement the deficit
                        drr_apply_cost(state, row, cost);

                        // decrement the task counters and update summary
                        cpu_state_deq_task(state, dsq);
                        return;
                    }
                }
                rr_bitmask >>= 1;
            }
        }
    }
}

void BPF_STRUCT_OPS(grid_running, struct task_struct *p) {
    bpf_debug("Running is called");
    u32 cpu = bpf_get_smp_processor_id();
    
    struct task_metadata *task_metadata = get_task_metadata(p);
    struct cpu_state *state = get_cpu_state();

    if (!task_metadata) {
        bpf_debug("Task without initialized metadata issued running callback");
        return;
    }

    if (!is_task_metadata_initialized(task_metadata)) {
		task_metadata->dsq_id = GRID_DSQ_ID(cpu, GRID_DSQ_COORD(DEFAULT_ROW, DEFAULT_COL));
	}

    u32 assigned_cpu = GRID_DSQ_CPU(task_metadata->dsq_id);
    if (assigned_cpu != cpu) {
        // bpf_printk("TASK NOT SCHEDULED ON CORRECT CPU: ASSIGNED CPU %d VS ACTUAL CPU %d", assigned_cpu, actual_cpu);
    }
    // bpf_printk("Task %d is running on CPU %d", p->pid, bpf_get_smp_processor_id());

    // Set the start time of the timeslice
    task_metadata->timeslice_start = bpf_ktime_get_ns();
    task_metadata->on_queue = false;

    bpf_debug("Running @ CPU #%d Summary %llx", bpf_get_smp_processor_id(), state->summary);
    bpf_debug("Running States: DSQ = %llx | Total Tasks - %u",
        task_metadata->dsq_id, state->total_tasks
    );

}

void BPF_STRUCT_OPS(grid_stopping, struct task_struct *p, bool runnable) {
    struct task_metadata *task_metadata = get_task_metadata(p);
	if (!task_metadata) {
        bpf_debug("Task without initialized metadata issued stopping callback");
        return;
    }

    if (!is_task_metadata_initialized(task_metadata)) {
		task_metadata->dsq_id = GRID_DSQ_COORD(DEFAULT_ROW, DEFAULT_COL);
        return;
	}

    dsq_id_t old_dsq = task_metadata->dsq_id;
    u32 old_col = GRID_DSQ_COL(old_dsq);
	u32 old_row = GRID_DSQ_ROW(old_dsq);

	u32 new_col = old_col;
	u32 new_row = old_row;

	if (!runnable) {
		if (p->scx.slice >= task_metadata->timeslice_duration / 2) {
			// Task only used half of its timeslice before yielding
			// Decrease the time slice given to the process
			new_col = DEC_COL(old_col);
		}
		else if (p->scx.slice == 0) {
			// Task used entire time slice and still can run,
			// Increaes the time slice given to the process
			new_col = INC_COL(old_col); 
		}
	}
	// Otherwise, the task was preempted. Cannot make any judgement

	task_metadata->dsq_id = GRID_DSQ_COORD(new_row, new_col);
}


SCX_OPS_DEFINE(grid_ops,
	       .init 		= (void*) grid_init,
	       .exit 		= (void*) grid_exit,
		   .enqueue 	= (void*) grid_enqueue,
		   .dequeue 	= (void*) grid_dequeue,
		   .dispatch 	= (void*) grid_dispatch,
		   .running 	= (void*) grid_running,
		   .stopping 	= (void*) grid_stopping,
           .flags = SCX_OPS_ENQ_LAST | SCX_OPS_ENQ_MIGRATION_DISABLED,
	       .name = "grid");