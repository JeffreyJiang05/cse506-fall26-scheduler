#ifndef TYPES_H
#define TYPES_H

#include <vmlinux.h>
#include <stdbool.h>

#include "util.h"
#include "config.h"
#include "macros.h"

typedef u64 dsq_id_t;

/** 
 * Compute DSQ ID based on the CPU, row, and column 
 */
#define GRID_DSQ_COORD(row, col) ((dsq_id_t) ((((dsq_id_t)(row)) << COL_BITS) | (col)))
#define GRID_DSQ_ID(cpu, coord) ((dsq_id_t) ((((dsq_id_t)(cpu)) << (ROW_BITS + COL_BITS)) | (coord)))


// Calculate row, column, and CPU a DSQ belongs to based on its ID
#define GRID_DSQ_COL(id) ((id) & MASK(COL_BITS))
#define GRID_DSQ_ROW(id) (((id) >> COL_BITS) & MASK(ROW_BITS))
#define GRID_DSQ_CPU(id) ((id) >> (ROW_BITS + COL_BITS))
#define GRID_DSQ_IDX(id) ((id) & MASK(ROW_BITS + COL_BITS))

#define for_each_grid_row(row) \
	for (u32 row = 0; row < GRID_NR_ROWS; ++row)

#define for_each_grid_col(col) \
	for (u32 col = 0; col < GRID_NR_COLS; ++col)

#define for_each_cpu_dsq(dsq_id, cpu) \
    for (u64 dsq_id = GRID_DSQ_ID((cpu), GRID_DSQ_COORD(0, 0)); \
         dsq_id <= GRID_DSQ_ID((cpu), GRID_DSQ_COORD(GRID_NR_ROWS - 1, GRID_NR_COLS - 1)); \
         ++dsq_id)

#define INC_ROW(row) ((row) < (GRID_NR_ROWS - 1) ? (row) + 1 : row)
#define DEC_ROW(row) ((row) > 0 ? (row) - 1 : row)
#define INC_COL(col) ((col) < (GRID_NR_COLS - 1) ? (col) + 1 : col)
#define DEC_COL(col) ((col) > 0 ? (col) - 1 : col)



#if GRID_NR_ROWS * GRID_NR_COLS <= 8
typedef u8 grid_summary_t;
#elif GRID_NR_ROWS * GRID_NR_COLS <= 16
typedef u16 grid_summary_t;
#elif GRID_NR_ROWS * GRID_NR_COLS <= 32
typedef u32 grid_summary_t;
#elif GRID_NR_ROWS * GRID_NR_COLS <= 64
typedef u64 grid_summary_t;
#endif

#define SUMMARY_ROW_COL(row, col) ((grid_summary_t) (1ULL << ((((dsq_id_t)(row)) << COL_BITS) | (col))))
#define DSQ_TO_SUMMARY(id) ((grid_summary_t) (1ULL << GRID_DSQ_IDX(id)))
#define SUMMARY_COL_MASK(col) ((grid_summary_t) ((MASK(GRID_NR_ROWS * GRID_NR_COLS) / MASK(GRID_NR_COLS)) << (col)))
#define SUMMARY_ROW_MASK(row) ((grid_summary_t) (MASK(GRID_NR_COLS)) << (((dsq_id_t)(row)) * GRID_NR_COLS))

// Define array of constants for quick runtime access

#define GRID_ROW_MASK_ARR_TEMPLATE(N, ...) [N] = SUMMARY_ROW_MASK(N),
static const grid_summary_t grid_row_masks[GRID_NR_ROWS] = {
	FOR_RANGE(0, GRID_NR_ROWS, GRID_ROW_MASK_ARR_TEMPLATE)
};
#undef GRID_ROW_MASK_ARR_TEMPLATE

#define GRID_COL_MASK_ARR_TEMPLATE(N, ...) [N] = SUMMARY_COL_MASK(N),
static const grid_summary_t grid_col_masks[GRID_NR_COLS] = {
	FOR_RANGE(0, GRID_NR_COLS, GRID_COL_MASK_ARR_TEMPLATE)
};
#undef GRID_COL_MASK_ARR_TEMPLATE

/*
 * Syscall sleep classification flags for Y-axis row placement.
 * Lower flag values map to lower row indices (higher latency priority).
 */
enum sleep_flag {
	SLEEP_FLAG_NONE     = 0,  /* No sleep event recorded (use default row) */
	SLEEP_FLAG_FUTEX    = 1,  /* Synchronization primitive (highest priority) */
	SLEEP_FLAG_POLL     = 2,  /* Event polling (epoll, poll, select) */
	SLEEP_FLAG_READ     = 3,  /* Blocking read I/O */
	SLEEP_FLAG_WRITE    = 4,  /* Blocking write I/O */
	SLEEP_FLAG_SLEEP    = 5,  /* Explicit sleep (nanosleep, clock_nanosleep) */
	SLEEP_FLAG_OTHER    = 6,  /* Any other blocking syscall */
	SLEEP_FLAG_MAX      = 7,  /* Sentinel / CPU-bound default */
};

/*
 * Row mapping from sleep flag.
 * sleep_flag_to_row[flag] gives the row index i for the task.
 */
static const u32 sleep_flag_to_row[8] = {
	[SLEEP_FLAG_NONE]   = DEFAULT_ROW,
	[SLEEP_FLAG_FUTEX]  = 0,  /* Critical sync → row 0 (highest priority) */
	[SLEEP_FLAG_POLL]   = 1,  /* Interactive I/O polling → row 1 */
	[SLEEP_FLAG_READ]   = 2,  /* Blocking read → row 2 */
	[SLEEP_FLAG_WRITE]  = 3,  /* Blocking write → row 3 */
	[SLEEP_FLAG_SLEEP]  = 5,  /* Explicit sleep → row 5 */
	[SLEEP_FLAG_OTHER]  = 6,  /* Misc blocking → row 6 */
	[SLEEP_FLAG_MAX]    = 7,  /* CPU-bound → row 7 (lowest priority) */
};

// Metadata in 2D grid
struct task_metadata {
	bool on_queue;
	// DSQ ID that task was last allocated on
	dsq_id_t dsq_id;
	// Time DSQ is enqueued to a run-queue
	u64 enqueue_time;
	// Start of timeslice
	u64 timeslice_start;
	// duration of timeslice
	u64 timeslice_duration;
	// Syscall classification flag set by tracepoint handlers
	u32 sleep_flag;
};

#define INIT_BPF_DSQ_ID SCX_DSQ_GLOBAL

static struct task_metadata task_metadata_init = { 
	.on_queue = false,
	.dsq_id = 0,
	.enqueue_time = 0,
	.timeslice_start = 0,
	.timeslice_duration = 0,
	.sleep_flag = SLEEP_FLAG_NONE
};

// helper functions/macros
static inline void task_metadata_initialize() {
	task_metadata_init.dsq_id = INIT_BPF_DSQ_ID;
}

static inline bool is_task_metadata_initialized(struct task_metadata *data) {
	return data->dsq_id != INIT_BPF_DSQ_ID;
}



struct row_metadata {
	// For deficit round robin (DRR)
	s32 deficit;
	u32 next_col_sched;
};

struct cpu_state {
	// High level summary of grid
	grid_summary_t summary;
	// Total tasks on CPU
	u32 total_tasks;
	// Info about rows of grid
	struct row_metadata rows[GRID_NR_ROWS];
	u32 task_counters[GRID_SIZE];
	// Next row for DRR
	u32 next_row_sched;
};

#define cpu_state_row_metadata(cpu_state, row) ((cpu_state)->rows[(row)])

static inline void cpu_state_enq_task(struct cpu_state *cpu_state, dsq_id_t dsq) {
	u32 idx = GRID_DSQ_IDX(dsq);
	grid_summary_t dsq_bitmask = DSQ_TO_SUMMARY(dsq);

	__sync_fetch_and_or(&cpu_state->summary, dsq_bitmask);
	__sync_fetch_and_add(&cpu_state->task_counters[idx], 1);
	__sync_fetch_and_add(&cpu_state->total_tasks, 1);
}

static inline void cpu_state_deq_task(struct cpu_state *cpu_state, dsq_id_t dsq) {
	u32 idx = GRID_DSQ_IDX(dsq);

	/* Underflow protection for total_tasks */
	if (cpu_state->total_tasks > 0) {
		__sync_fetch_and_sub(&cpu_state->total_tasks, 1);
	}

	/* Underflow protection for task_counters */
	if (cpu_state->task_counters[idx] > 0) {
		u32 prev = __sync_fetch_and_sub(&cpu_state->task_counters[idx], 1);
		if (prev == 1) {
			/* Counter reached 0: atomically clear summary bit */
			__sync_fetch_and_and(&cpu_state->summary, ~DSQ_TO_SUMMARY(dsq));
			u32 row = GRID_DSQ_ROW(dsq);
			if (row < GRID_NR_ROWS && !(cpu_state->summary & grid_row_masks[row])) {
				cpu_state->rows[row].deficit = 0;
			}
		}
	} else {
		/* Counter was already 0: ensure summary bit is cleared if set */
		__sync_fetch_and_and(&cpu_state->summary, ~DSQ_TO_SUMMARY(dsq));
	}
}

#endif
