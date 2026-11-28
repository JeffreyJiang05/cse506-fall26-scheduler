/* Grid scheduler — Multi-Dimensional MLFQ
 *
 * Implements the full Design.md specification:
 *   - 8×8 per-CPU dispatch grid with summary bitmap
 *   - X-axis time-slice feedback (promote / demote / retain)
 *   - Y-axis latency classification via syscall tracepoint flags
 *   - Deficit Round Robin (inter-row) + Round Robin (intra-row) dispatch
 *   - Wakeup preemption based on latency target vs remaining slice
 *   - Multi-core work stealing when local grid is empty
 *   - Ring-buffer telemetry event channel
 */

#include <scx/common.bpf.h>

#include "types.h"
#include "stats.bpf.h"

#include "debug.bpf.h"

char _license[] SEC("license") = "GPL";
UEI_DEFINE(uei);

// *========================== Useful Macros ==========================* //

#define DRR_MAX_ITERATIONS 3
#define WORK_STEAL_MAX_CPUS 4  /* Max CPUs to probe during work stealing */

/**
 * Assigns the task to a specific CPU. Updates the task_metadata->dsq_id
 * to include the given processor ID.
 */
#define task_metadata_assign_to_cpu(task_metadata, cpu_id) do { \
    (task_metadata)->dsq_id = GRID_DSQ_ID((cpu_id), GRID_DSQ_IDX((task_metadata)->dsq_id)); \
} while(0)

/**
 * Assigns the task to the local CPU. Updates the task_metadata->dsq_id
 * to include the current local processor ID.
 */
#define task_metadata_assign_to_local_cpu(task_metadata) do { \
    u32 __cpu = bpf_get_smp_processor_id(); \
    task_metadata_assign_to_cpu(task_metadata, __cpu); \
} while(0)

// *========================== Userspace Input ==========================* //

/**
 * Data to be populated by userspace
 */
const volatile u32 nr_cpus; // Number of CPU cores
const volatile u32 latency[GRID_NR_ROWS]; // Latency target for each row in grid (us)
const volatile u16 slices[GRID_NR_COLS]; // Time slices for each column in grid (ms)
const volatile u16 drr_quanta[GRID_NR_ROWS]; // Quanta for DRR

static void debug_printk_config() {
    bpf_debug("nr_cpus = %d", nr_cpus);
    for (u32 i = 0; i < GRID_NR_ROWS; ++i) {
        bpf_debug("latency[%u] = %u us", i, latency[i]);
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

/*
 * Per-CPU state: global array so work stealing can read
 * remote CPU states for cross-CPU visibility
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, u32);
    __type(value, struct cpu_state);
    __uint(max_entries, 1024); /* upper bound on nr_cpus */
} grid_cpu_state SEC(".maps");

static inline struct cpu_state *get_cpu_state_of(u32 cpu) {
    return bpf_map_lookup_elem(&grid_cpu_state, &cpu);
}

static inline struct cpu_state *get_cpu_state() {
    u32 cpu = bpf_get_smp_processor_id();
    return get_cpu_state_of(cpu);
}

/*
 * Per-CPU telemetry: task counts for live user-space polling.
 * Index = cpu_id. Value = total runnable tasks on that CPU.
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, u32);
    __type(value, u32);
    __uint(max_entries, 1);
} grid_telemetry SEC(".maps");

static inline void telemetry_update_task_count(struct cpu_state *state) {
    u32 key = 0;
    u32 *count = bpf_map_lookup_elem(&grid_telemetry, &key);
    if (count)
        *count = state->total_tasks;
}

// *========================== Syscall Tracepoint Handlers ==========================* //
// Y-Axis Classification: attach to sleep-inducing syscalls to tag tasks

/*
 * Helper: write a sleep classification flag to the task's metadata.
 * Called from tracepoint handlers when a task enters a blocking syscall.
 */
static inline void set_task_sleep_flag(struct task_struct *p, u32 flag) {
    struct task_metadata *meta = get_task_metadata(p);
    if (meta)
        meta->sleep_flag = flag;
}

/*
 * sys_enter_futex — synchronization primitives (mutexes, condvars).
 * Maps to row 0 (highest latency priority).
 */
SEC("tp_btf/sys_enter")
int BPF_PROG(grid_sys_enter_futex, struct pt_regs *regs, long id) {
    /* __NR_futex = 202 on x86_64 */
    if (id != 202)
        return 0;
    struct task_struct *p = (struct task_struct *)bpf_get_current_task_btf();
    set_task_sleep_flag(p, SLEEP_FLAG_FUTEX);
    return 0;
}

/*
 * sys_enter for read/write/poll classification.
 * We use a single sys_enter handler and switch on the syscall number
 * to minimize the number of attached programs.
 */
SEC("tp_btf/sys_enter")
int BPF_PROG(grid_sys_enter_io, struct pt_regs *regs, long id) {
    struct task_struct *p;
    u32 flag;

    switch (id) {
    case 0:    /* __NR_read */
    case 17:   /* __NR_pread64 */
    case 19:   /* __NR_readv */
        flag = SLEEP_FLAG_READ;
        break;
    case 1:    /* __NR_write */
    case 18:   /* __NR_pwrite64 */
    case 20:   /* __NR_writev */
        flag = SLEEP_FLAG_WRITE;
        break;
    case 7:    /* __NR_poll */
    case 23:   /* __NR_select */
    case 232:  /* __NR_epoll_wait */
    case 281:  /* __NR_epoll_pwait */
    case 441:  /* __NR_epoll_pwait2 */
    case 270:  /* __NR_pselect6 */
    case 271:  /* __NR_ppoll */
        flag = SLEEP_FLAG_POLL;
        break;
    case 35:   /* __NR_nanosleep */
    case 230:  /* __NR_clock_nanosleep */
        flag = SLEEP_FLAG_SLEEP;
        break;
    default:
        return 0;
    }

    p = (struct task_struct *)bpf_get_current_task_btf();
    set_task_sleep_flag(p, flag);
    return 0;
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

    report_event(EVENT_SCHED_START);

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

    report_event(EVENT_SCHED_END);

    UEI_RECORD(uei, ei);
}

/**
 * Checks whether a task is permitted to run on a given CPU based on its affinity mask.
 */
static inline bool task_can_run_on_cpu(struct task_struct *p, u32 cpu) {
    const cpumask_t *cpus_ptr = BPF_CORE_READ(p, cpus_ptr);
    if (!cpus_ptr)
        return true;
    u32 word = cpu / 64;
    u32 bit = cpu % 64;
    if (word >= 8)
        return false;
    unsigned long mask = BPF_CORE_READ(cpus_ptr, bits[word]);
    return (mask & (1UL << bit)) != 0;
}

/**
 * Initial Task Placement & Wakeup CPU Selection (Design §Initial Task Placement):
 * When a task wakes up or is created/forked, select the CPU with the fewest
 * runnable tasks currently in its dispatch grid, respecting CPU affinity masks.
 * Starts search from prev_cpu to preserve cache locality on ties.
 */
s32 BPF_STRUCT_OPS(grid_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags) {
    u32 total = nr_cpus;
    if (total == 0 || total > 64)
        total = 64;

    s32 best_cpu = -1;
    u32 min_tasks = (u32)-1;

    for (u32 i = 0; i < 64; ++i) {
        if (i >= total)
            break;
        u32 cand_cpu = ((u32)prev_cpu + i) % total;

        if (!task_can_run_on_cpu(p, cand_cpu))
            continue;

        struct cpu_state *state = get_cpu_state_of(cand_cpu);
        if (!state)
            continue;

        /* Atomic read prevents tearing / data races with concurrent enqueues/dequeues */
        u32 tasks = READ_ONCE(state->total_tasks);

        if (tasks == 0) {
            /* Found an empty dispatch grid: immediately select this CPU */
            return (s32)cand_cpu;
        }

        if (tasks < min_tasks) {
            min_tasks = tasks;
            best_cpu = (s32)cand_cpu;
        }
    }

    if (best_cpu >= 0)
        return best_cpu;

    return prev_cpu;
}

/**
 * Triggers when a task is being enqueued. Two possible cases:
 * 
 * (1) The task is new to the scheduler and its per-task data is not
 *     initialized. Set to default row and column positions.
 * (2) The task has been scheduled before — use its computed (i_next, c_next).
 * 
 * On wakeup: also checks if running task should be preempted based on
 * latency target comparison (Design §Wakeup Preemption Trigger).
 */
void BPF_STRUCT_OPS(grid_enqueue, struct task_struct *p, u64 enq_flags) {
    s32 task_cpu = scx_bpf_task_cpu(p);
    u32 cpu = (task_cpu >= 0 && task_cpu < (s32)nr_cpus) ? (u32)task_cpu : bpf_get_smp_processor_id();

    struct task_metadata *task_metadata = get_task_metadata(p);
    if (!task_metadata) {
        // Failed to allocate task metadata!
        scx_bpf_error("Failed to allocate task metadata");
        return;
    }

    /*
     * If the task was already marked on_queue (e.g. re-enqueue via
     * SCX_ENQ_REENQ or migration without an explicit dequeue callback),
     * remove it from its previous CPU/DSQ first to prevent double-counting.
     */
    if (task_metadata->on_queue) {
        u32 prev_cpu = GRID_DSQ_CPU(task_metadata->dsq_id);
        struct cpu_state *prev_state = get_cpu_state_of(prev_cpu);
        if (prev_state) {
            cpu_state_deq_task(prev_state, task_metadata->dsq_id);
            if (prev_cpu == cpu)
                telemetry_update_task_count(prev_state);
        }
        task_metadata->on_queue = false;
    }

    dsq_id_t old_dsq = is_task_metadata_initialized(task_metadata) ? task_metadata->dsq_id : 0;

    // If the task's metadata has not been scheduled yet, set to default
    if (!is_task_metadata_initialized(task_metadata)) {
		task_metadata->dsq_id = GRID_DSQ_COORD(DEFAULT_ROW, DEFAULT_COL);
	}

    /*
     * Y-Axis: On wakeup, read the sleep_flag set by syscall tracepoint
     * handlers to compute the target row index.
     */
    if (enq_flags & SCX_ENQ_WAKEUP) {
        u32 flag = task_metadata->sleep_flag;
        u32 new_row;

        /* Bounds check for verifier */
        if (flag < 8)
            new_row = sleep_flag_to_row[flag];
        else
            new_row = DEFAULT_ROW;

        u32 cur_col = GRID_DSQ_COL(task_metadata->dsq_id);
        task_metadata->dsq_id = GRID_DSQ_COORD(new_row, cur_col);

        /* Reset flag after consumption */
        task_metadata->sleep_flag = SLEEP_FLAG_NONE;
    }

    /*
     * Extract coordinate cleanly (0..63) to prevent previous CPU ID
     * bits from nesting into the new DSQ ID across migrations.
     */
    u32 coord = GRID_DSQ_IDX(task_metadata->dsq_id);
    dsq_id_t dsq = GRID_DSQ_ID(cpu, coord);
    task_metadata->dsq_id = dsq;

    struct cpu_state *state = get_cpu_state_of(cpu);
    if (!state) return;

    // Determine the timeslice allocated to task
    u32 col = GRID_DSQ_COL(dsq);
    if (col >= GRID_NR_COLS) col = DEFAULT_COL;
    task_metadata->timeslice_duration = slices[col] * NS;

    // Update enqueue time
    task_metadata->enqueue_time = bpf_ktime_get_ns();

    // Set flag that it is on the queue
    task_metadata->on_queue = true;
    
    // Insert into DSQ first
    scx_bpf_dsq_insert(p, 
        dsq, 
        task_metadata->timeslice_duration, 
        0);

    // Update CPU summary information and task counters after insertion
    cpu_state_enq_task(state, dsq);

    // If enqueued to a remote CPU, kick it to trigger immediate dispatch
    if (cpu != bpf_get_smp_processor_id()) {
        scx_bpf_kick_cpu(cpu, 0);
    }

    bpf_debug("CPU #%u enqueued task %d (%s) on DSQ ID %llx", cpu, p->pid, p->comm, task_metadata->dsq_id);
    bpf_debug("Set summary flag to %llx", state->summary);

    // Report enqueue event to ring buffer
    report_event(EVENT_TASK_ENQUEUE,
        .pid = p->pid,
        .old = old_dsq,
        .new = task_metadata->dsq_id);

    /*
     * Wakeup Preemption Trigger (Design §Task Lifecycle & Preemption Rules):
     * If T_rem of running task > l_wake of this waking task, preempt.
     * Evaluated against the locally executing task on this CPU.
     */
    if ((enq_flags & SCX_ENQ_WAKEUP) && cpu == bpf_get_smp_processor_id()) {
        u32 wake_row = GRID_DSQ_ROW(task_metadata->dsq_id);
        if (wake_row < GRID_NR_ROWS) {
            u64 l_wake = (u64)latency[wake_row] * NS_PER_US;
            bool should_preempt = false;
            u32 victim_pid = 0;

            struct task_struct *curr = (struct task_struct *)bpf_get_current_task_btf();
            if (curr && curr != p) {
                struct task_metadata *curr_meta = get_task_metadata(curr);
                if (curr_meta && curr_meta->timeslice_start > 0) {
                    u32 curr_row = GRID_DSQ_ROW(curr_meta->dsq_id);
                    /*
                     * Only preempt if waking task has higher latency priority
                     * (strictly lower row index) than the currently running task,
                     * AND the running task's remaining timeslice exceeds waking
                     * task's latency target (T_rem > l_wake).
                     */
                    if (wake_row < curr_row) {
                        u64 now = bpf_ktime_get_ns();
                        u64 elapsed = (now > curr_meta->timeslice_start) ?
                                      (now - curr_meta->timeslice_start) : 0;
                        u64 t_rem = (curr_meta->timeslice_duration > elapsed) ?
                                    (curr_meta->timeslice_duration - elapsed) : 0;
                        if (t_rem > l_wake) {
                            should_preempt = true;
                            victim_pid = curr->pid;
                        }
                    }
                }
            }

            if (should_preempt) {
                /* Request preemption on this CPU */
                scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
                bpf_debug("Wakeup preemption: wake_row=%u latency=%llu preempting pid=%u",
                          wake_row, l_wake, victim_pid);

                // Report preemption event
                report_event(EVENT_TASK_PREEMPT,
                    .pid = p->pid,
                    .dsq = task_metadata->dsq_id,
                    .victim_pid = victim_pid);
            }
        }
    }

    // Update telemetry
    telemetry_update_task_count(state);
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

    // Update the CPU state WHERE THE TASK WAS ACTUALLY ENQUEUED
    u32 target_cpu = GRID_DSQ_CPU(task_metadata->dsq_id);
    struct cpu_state *state = get_cpu_state_of(target_cpu);
    if (!state) return;
    cpu_state_deq_task(state, task_metadata->dsq_id);

    // Report cancelled event (task removed from DSQ before dispatch)
    report_event(EVENT_TASK_CANCELLED,
        .pid = p->pid,
        .dsq = task_metadata->dsq_id);

    // Update telemetry
    if (target_cpu == bpf_get_smp_processor_id()) {
        telemetry_update_task_count(state);
    }
}

// *========================== DRR Dispatch ==========================* //

#define drr_can_schedule(cpu_state, row, cost) (cpu_state_row_metadata((cpu_state), (row)).deficit >= (cost))

#define DRR_MAX_DEFICIT(row) (drr_quanta[(row)] + slices[GRID_NR_COLS - 1])

#define drr_apply_quantum(cpu_state, row, quantum) do { \
	(cpu_state)->rows[(row)].deficit += (quantum); \
	if ((cpu_state)->rows[(row)].deficit > (s32)DRR_MAX_DEFICIT(row)) \
		(cpu_state)->rows[(row)].deficit = (s32)DRR_MAX_DEFICIT(row); \
} while (0)

#define drr_apply_cost(cpu_state, row, cost) do { \
    (cpu_state)->rows[(row)].deficit -= (cost); \
} while (0)

/*
 * Work Stealing helper (Design §Multi-Core Load Balancing):
 * If local grid is completely empty, steal a task from a remote CPU.
 * Marked __noinline so the BPF verifier verifies it in an isolated frame,
 * preventing state space explosion in grid_dispatch.
 */
static __noinline bool try_work_steal(s32 cpu, struct cpu_state *state) {
    u32 total = nr_cpus;
    if (total == 0)
        return false;
    if (total > 64)
        total = 64;

    u32 scanned_candidates = 0;

    for (u32 i = 0; i < 64; ++i) {
        if (i >= total)
            break;
        u32 remote_cpu = ((u32)cpu + 1 + i) % total;
        if (remote_cpu == (u32)cpu)
            continue;

        struct cpu_state *remote = get_cpu_state_of(remote_cpu);
        if (!remote || !READ_ONCE(remote->summary))
            continue;

        scanned_candidates++;

        /* Start scanning from remote CPU's next_row_sched for inter-row fairness */
        u32 remote_start_row = READ_ONCE(remote->next_row_sched);
        if (remote_start_row >= GRID_NR_ROWS) remote_start_row = 0;

        for (u32 ri = 0; ri < GRID_NR_ROWS; ++ri) {
            u32 row = (remote_start_row + ri) % GRID_NR_ROWS;
            if (row >= GRID_NR_ROWS) continue;

            grid_summary_t remote_sum = READ_ONCE(remote->summary);
            grid_summary_t row_summary = remote_sum & grid_row_masks[row];
            if (!row_summary)
                continue;

            for (u32 col = 0; col < GRID_NR_COLS; ++col) {
                if (!(row_summary & SUMMARY_ROW_COL(row, col)))
                    continue;

                dsq_id_t remote_dsq = GRID_DSQ_ID(remote_cpu, GRID_DSQ_COORD(row, col));
                if (scx_bpf_dsq_move_to_local(remote_dsq)) {
                    bpf_debug("CPU #%d stole task from CPU #%u DSQ (%u,%u)",
                              cpu, remote_cpu, row, col);
                    cpu_state_deq_task(remote, remote_dsq);

                    telemetry_update_task_count(state);

                    // Report work stealing / task migration event
                    report_event(EVENT_TASK_MIGRATE,
                        .pid = 0,
                        .src_cpu = remote_cpu,
                        .dst_cpu = cpu,
                        .dsq = remote_dsq);

                    // Report dispatch event
                    report_event(EVENT_TASK_DISPATCH,
                        .pid = 0,
                        .dsq = remote_dsq);
                    return true;
                }
            }
        }
        if (scanned_candidates >= WORK_STEAL_MAX_CPUS)
            break;
    }
    return false;
}

/*
 * grid_dispatch: Select a task to run using the two-tier fairness engine.
 *   - Inter-row: Deficit Round Robin across rows 0..7
 *   - Intra-row: Standard Round Robin across columns 0..7
 *
 * If the local grid is empty, attempt work stealing from remote CPUs.
 */
void BPF_STRUCT_OPS(grid_dispatch, s32 cpu, struct task_struct *prev) {
    struct cpu_state *state = get_cpu_state();
    if (!state) return;

    /*
     * Work Stealing (Design §Multi-Core Load Balancing):
     * If local grid is completely empty, steal from remote CPUs.
     */
    if (!state->summary) {
        try_work_steal(cpu, state);
        return;
    }

    bpf_debug("CPU #%d Summary %llx", cpu, state->summary);

    /*
     * DRR dispatch: iterate up to DRR_MAX_ITERATIONS rounds.
     * Each round starts scanning from next_row_sched for fairness.
     */
    for (u32 iter = 0; iter < DRR_MAX_ITERATIONS; ++iter) {
        u32 start_row = state->next_row_sched;
        if (start_row >= GRID_NR_ROWS) start_row = 0;

        for (u32 ri = 0; ri < GRID_NR_ROWS; ++ri) {
            u32 row = (start_row + ri) % GRID_NR_ROWS;
            if (row >= GRID_NR_ROWS) continue; /* verifier hint */

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
            if (start_col >= GRID_NR_COLS) start_col = 0;

            for (u32 ci = 0; ci < GRID_NR_COLS; ++ci) {
                u32 col = (start_col + ci) % GRID_NR_COLS;
                if (col >= GRID_NR_COLS) continue; /* verifier hint */

                if (!(row_summary & SUMMARY_ROW_COL(row, col)))
                    continue;

                u32 cost = slices[col];

                bpf_debug("Found a bit at row %u and col %u.", row, col);

                // Check if the row can schedule a task with this cost
                if (drr_can_schedule(state, row, cost)) {
                    // Schedule the task by moving it into the local DSQ
                    dsq_id_t dsq = GRID_DSQ_ID(cpu, GRID_DSQ_COORD(row, col));
                    bpf_debug("CPU #%d: Attempting to move task from DSQ %llx", cpu, dsq);
                    if (!scx_bpf_dsq_move_to_local(dsq)) {
                        continue;
                    }

                    bpf_debug("Successfully scheduled a task");

                    // Successfully scheduled the task
                    // Decrement the deficit
                    drr_apply_cost(state, row, cost);

                    // decrement the task counters and update summary
                    cpu_state_deq_task(state, dsq);

                    // Update intra-row RR: next column to schedule
                    cpu_state_row_metadata(state, row).next_col_sched = (col + 1) % GRID_NR_COLS;

                    // Update inter-row DRR: next row to start from
                    state->next_row_sched = (row + 1) % GRID_NR_ROWS;

                    // Report dispatch event
                    report_event(EVENT_TASK_DISPATCH,
                        .pid = 0,
                        .dsq = dsq);

                    // Update telemetry
                    telemetry_update_task_count(state);
                    return;
                }
            }

            /*
             * If after scanning all columns in the row, the row has no
             * remaining tasks in the summary bitmap, reset deficit to 0.
             */
            if (!(state->summary & grid_row_masks[row])) {
                cpu_state_row_metadata(state, row).deficit = 0;
            }
        }
    }

    /*
     * If all iterations finished without scheduling a task, advance
     * the row pointer so subsequent dispatches try the next row.
     */
    state->next_row_sched = (state->next_row_sched + 1) % GRID_NR_ROWS;
}

/*
 * grid_running: Called when a task begins executing on a CPU.
 * Records the timeslice start time for X-axis feedback computation.
 */
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

    // Set the start time of the timeslice
    task_metadata->timeslice_start = bpf_ktime_get_ns();
    task_metadata->on_queue = false;
    task_metadata_assign_to_local_cpu(task_metadata);

    bpf_debug("Running @ CPU #%d Summary %llx", bpf_get_smp_processor_id(), state ? state->summary : 0);
    bpf_debug("Running States: DSQ = %llx | Total Tasks - %u",
        task_metadata->dsq_id, state ? state->total_tasks : 0
    );

}

/*
 * grid_stopping: Called when a task stops running (deschedule event).
 *
 * Implements the X-Axis Time Slice Feedback policy (Design §Core Scheduling):
 *   - T >= t_c         → demotion:  c_next = min(c+1, 7)
 *   - T < t_c / 2      → promotion: c_next = max(c-1, 0)
 *   - t_c/2 <= T < t_c → retention: c_next = c
 *
 * Row (Y-axis) is set here to default; it will be overridden on wakeup
 * by the syscall tracepoint classification.
 */
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

    /*
     * X-Axis Feedback: Measure actual execution time T and compare
     * against the assigned time slice t_c.
     */
    u64 now = bpf_ktime_get_ns();
    u64 T = 0;
    if (task_metadata->timeslice_start > 0 && now > task_metadata->timeslice_start)
        T = now - task_metadata->timeslice_start;

    u64 t_c = task_metadata->timeslice_duration;

    if (t_c > 0) {
        if (T >= t_c) {
            /* Demotion: task consumed full quantum (CPU-bound) */
            new_col = INC_COL(old_col);
        } else if (!runnable && T < t_c / 2) {
            /* Promotion: task voluntarily yielded early (interactive/I/O-bound) */
            new_col = DEC_COL(old_col);
        } else {
            /* Retention: task preempted early or consumed between [t_c/2, t_c) */
            new_col = old_col;
        }
    }

    /*
     * If the task is going to sleep (!runnable), keep the row as-is;
     * it will be re-classified on wakeup by the syscall tracepoint
     * handler. If it's still runnable (preempted), keep the same row.
     */
    u32 cpu = bpf_get_smp_processor_id();
    task_metadata->dsq_id = GRID_DSQ_ID(cpu, GRID_DSQ_COORD(new_row, new_col));

    if (runnable) {
        // Task stopped while still runnable (preempted / time slice expired)
        report_event(EVENT_TASK_PREEMPT,
            .pid = p->pid,
            .dsq = old_dsq,
            .victim_pid = p->pid);
    }

    bpf_debug("Stopping: pid=%d T=%llu t_c=%llu old_col=%u new_col=%u",
              p->pid, T, t_c, old_col, new_col);
}


SCX_OPS_DEFINE(grid_ops,
	       .select_cpu	= (void*) grid_select_cpu,
	       .init 		= (void*) grid_init,
	       .exit 		= (void*) grid_exit,
		   .enqueue 	= (void*) grid_enqueue,
		   .dequeue 	= (void*) grid_dequeue,
		   .dispatch 	= (void*) grid_dispatch,
		   .running 	= (void*) grid_running,
		   .stopping 	= (void*) grid_stopping,
           .flags = SCX_OPS_ENQ_LAST,
	       .name = "grid");
