# Grid: A Multi-Dimensional MLFQ Scheduler — Design Specification

## System Overview

The Grid Scheduler is a Linux CPU scheduler built on eBPF and the `sched_ext` (`scx`) framework. It evolves traditional Multilevel Feedback Queue (MLFQ) scheduling by decoupling execution time slice length from process latency requirements into an orthogonal, multi-dimensional dispatch grid.

The design eliminates rigid scalar priorities by decomposing task behavior across two independent axes:
1. **Time-Slice Axis (X-Axis):** Dynamically adjusts allotted CPU execution duration based on runtime execution behavior.
2. **Latency-Target Axis (Y-Axis):** Dynamically classifies task latency sensitivity based on kernel sleep and synchronization system call patterns.

---

## Grid Geometry & Data Structures

### Two-Dimensional Dispatch Queue Matrix

Each CPU maintains an $8 \times 8$ matrix consisting of 64 discrete eBPF Dispatch Queues (DSQs). A queue cell at row index $i \in [0, 7]$ and column index $j \in [0, 7]$ binds tasks to latency requirement $l_i$ and time slice duration $t_j$.

| Dimension   | Axis   | Index Range    | Managed Property            | Policy Engine                          |
| :---------- | :----- | :------------- | :-------------------------- | :------------------------------------- |
| **Columns** | X-Axis | $j \in [0, 7]$ | Time Slice Duration ($t_j$) | Execution Time Feedback ($T$ vs $t_c$) |
| **Rows**    | Y-Axis | $i \in [0, 7]$ | Latency Target ($l_i$)      | Syscall Tracepoint Flag Analysis       |

### DSQ Identifier Encoding

DSQ identifiers are 64-bit integers (`dsq_id_t`) uniquely identifying the target CPU and grid cell:

$$\text{dsq\_id} = (\text{cpu} \ll 6) \mid (i \ll 3) \mid j$$

* **CPU Identifier:** Bits $[63 \dots 6]$ encode the processor ID ($\text{cpu} \in [0, 1023]$).
* **Grid Coordinate Index:** Bits $[5 \dots 0]$ encode the linear cell index $\text{coord} = (i \ll 3) \mid j \in [0, 63]$.
* **Row Index ($i$):** Bits $[5 \dots 3]$ encode row $i \in [0, 7]$.
* **Column Index ($j$):** Bits $[2 \dots 0]$ encode column $j \in [0, 7]$.

### Grid State Summary Bitmap

To eliminate non-deterministic queue iteration within eBPF scheduling hot-paths, the active state of all 64 queues is aggregated into a single 64-bit unsigned integer bitmap (`u64 summary`).

* **Bit Encoding:** Each bit $k \in [0, 63]$ in `summary` corresponds to queue cell $(i, j)$ where $k = i \times 8 + j$.
* **Bit Value:** A bit set to `1` indicates that queue $(i, j)$ contains at least one runnable task; a bit set to `0` indicates an empty queue.
* **Row & Column Bitmasks:** Precomputed bitmask lookup arrays (`grid_row_masks[8]` and `grid_col_masks[8]`) allow instant single-instruction row-level and column-level occupancy checks without bit-shifting loops.

### State Tracking & Metadata Structures

```c
struct row_metadata {
    s32 deficit;         /* Accumulated Deficit Round Robin credit */
    u32 next_col_sched;  /* Intra-row round-robin column pointer (0..7) */
};

struct cpu_state {
    grid_summary_t summary;               /* 64-bit occupancy bitmap */
    u32 total_tasks;                      /* Total runnable tasks in this CPU's grid */
    struct row_metadata rows[8];          /* Per-row DRR state and RR pointers */
    u32 task_counters[64];                /* Exact runnable task count per DSQ cell */
    u32 next_row_sched;                   /* Inter-row DRR cursor pointer (0..7) */
};

struct task_metadata {
    bool on_queue;                        /* True if task is currently inside a DSQ */
    dsq_id_t dsq_id;                      /* Current or last assigned DSQ ID */
    u64 enqueue_time;                     /* Nanosecond timestamp when enqueued */
    u64 timeslice_start;                  /* Nanosecond timestamp when execution began */
    u64 timeslice_duration;               /* Assigned timeslice quantum (ns) */
    u32 sleep_flag;                       /* Syscall classification flag */
};
```

### Atomic Synchronization & Concurrency Invariants

To avoid phantom occupancy bits, counter underflows, and race conditions between concurrent enqueues, dispatches, remote work stealing, and task placement:

1. **Strict Enqueue Ordering:** In `grid_enqueue`, tasks are physically inserted into the kernel DSQ via `scx_bpf_dsq_insert` **before** the occupancy bit is set in `state->summary`. This guarantees that any core observing a set bit in `summary` will never encounter an empty DSQ.
2. **Atomic Counter-First Update:** When setting occupancy, `cpu_state_enq_task` increments `task_counters[coord]` and `total_tasks` using atomic additions (`__sync_fetch_and_add`) before asserting the bit in `summary` (`__sync_fetch_and_or`).
3. **Underflow-Safe Atomic Dequeue with Double-Check:** `cpu_state_deq_task` decrements `task_counters[coord]` and `total_tasks` using atomic Compare-And-Swap (`__sync_val_compare_and_swap`) loops. When the cell counter transitions from $1 \to 0$, it atomically clears the bit in `summary` (`__sync_fetch_and_and`). It then performs a double-check on `task_counters[coord] > 0`: if a concurrent enqueue incremented the counter in the interleaved window, the bit is immediately reasserted to heal the bitmap.
4. **Local-Only Row Deficit Mutation:** Per-row scheduling state (`rows[row].deficit` and `rows[row].next_col_sched`) is exclusively mutated by the local CPU in `grid_dispatch`. Remote CPUs executing `cpu_state_deq_task` during work stealing or re-enqueue cleanup only touch atomic counters (`task_counters`, `total_tasks`, `summary`) and never write to `rows`, completely eliminating cross-CPU data races on DRR deficits.
5. **Cross-CPU Read Safety:** In cross-core operations (`grid_select_cpu` and `try_work_steal`), remote metrics (`total_tasks`, `summary`, `next_row_sched`) are loaded using atomic relaxed reads (`READ_ONCE`), preventing compiler tearing and cache incoherencies.

---

## User-Space Configuration Interface

The user-space launching application (`src/sched.c`), built with `libbpf` and `scx` helper libraries, configures kernel-side scheduling parameters at startup via eBPF maps and provides observability channels.

### Configuration Vectors

The scheduler exposes three key configuration vectors across the 8 rows and 8 columns:

| Parameter Vector          | Symbol | Dimensions                    | Description & Unit                                        | Default Progression                                   |
| :------------------------ | :----- | :---------------------------- | :-------------------------------------------------------- | :---------------------------------------------------- |
| **Target Latency Vector** | $l_i$  | Row Array ($i \in [0, 7]$)    | Maximum latency threshold assigned to row $i$ ($\mu$s)     | $\{250, 500, 1000, 2000, 4000, 8000, 16000, 32000\}$ ($250\,\mu\text{s}$ to $32\,\text{ms}$) |
| **Time Slice Vector**     | $t_j$  | Column Array ($j \in [0, 7]$) | Allotted CPU execution quantum assigned to column $j$ (ms) | $\{1, 2, 4, 8, 16, 32, 64, 128\}$ (Powers of 2)      |
| **DRR Quantum Vector**    | $q_i$  | Row Array ($i \in [0, 7]$)    | Deficit Round Robin replenishment quantum for row $i$     | $\{512, 384, 256, 192, 128, 96, 64, 32\}$ (Harmonic) |

### Command-Line Arguments

* `-l <file>`: Load custom Target Latency Vector from file.
* `-q <file>`: Load custom Time Slice Vector from file.
* `-d <file>`: Load custom DRR Quanta Vector from file.
* `-o <file>`: Stream discrete scheduling event log to a structured text file with decomposed `(cpu, row, col)` coordinates.
* `-t`: Launch interactive full-screen `ncurses` Terminal User Interface (TUI).
* `-h`: Display usage help and exit.

---

## Core Scheduling & Dispatch Policies

### Time Slice Length Policy (X-Axis Feedback)

When a task completes an execution turn on a CPU (descheduled or preempted), the scheduler measures actual execution time $T = t_{\text{stop}} - t_{\text{start}}$ against assigned timeslice $t_c$ (where $c$ is the current column index). The next column assignment is computed as follows:

1. **Demotion (Rightward Shift):** If $T \ge t_c$, the task consumed its full quantum (CPU-bound). The next column assignment is:
   $$c_{next} = \min(c + 1, \text{NR\_COLS} - 1)$$
2. **Promotion (Leftward Shift):** If $!runnable$ (task voluntarily yielded / blocked) and $T < \frac{t_c}{2}$, the task yielded early (interactive / I/O-bound). The next column assignment is:
   $$c_{next} = \max(c - 1, 0)$$
3. **Retention (No Shift):** If the task was preempted early while still runnable, or consumed execution time between $\left[\frac{t_c}{2}, t_c\right)$, it retains its column:
   $$c_{next} = c$$

### Latency Requirement Policy (Y-Axis Classification)

Row placement $i$ is determined dynamically by tracking blocking system calls via eBPF tracepoints.

#### Tracepoint Flag Classification

| Sleep Flag Enum       | Value | Assigned Row | Intercepted System Calls                                  | Latency Characterization        |
| :-------------------- | :---- | :----------- | :-------------------------------------------------------- | :------------------------------ |
| **SLEEP_FLAG_FUTEX**  | 1     | Row 0        | `sys_enter_futex`, `sys_enter_futex_waitv`                | Critical lock / synchronization |
| **SLEEP_FLAG_POLL**   | 2     | Row 1        | `sys_enter_epoll_wait`, `sys_enter_poll`, `select`, etc.  | Interactive event multiplexing  |
| **SLEEP_FLAG_READ**   | 3     | Row 2        | `sys_enter_read`, `sys_enter_pread64`, `readv`, etc.      | Blocking read I/O               |
| **SLEEP_FLAG_WRITE**  | 4     | Row 3        | `sys_enter_write`, `sys_enter_pwrite64`, `writev`, etc.   | Blocking write I/O              |
| **SLEEP_FLAG_NONE**   | 0     | Row 3        | Default row (`DEFAULT_ROW = 3`)                           | Unclassified / initial tasks    |
| **SLEEP_FLAG_SLEEP**  | 5     | Row 5        | `sys_enter_nanosleep`, `sys_enter_clock_nanosleep`        | Explicit timed delay            |
| **SLEEP_FLAG_OTHER**  | 6     | Row 6        | Other unclassified blocking system calls                  | Miscellaneous sleep             |
| **SLEEP_FLAG_MAX**    | 7     | Row 7        | Sentinel / CPU-bound baseline                             | Lowest latency priority         |

#### Lifecycle Ingestion
1. **Syscall Entry:** When a task enters a blocking syscall, the corresponding tracepoint handler sets `meta->sleep_flag`.
2. **Task Wakeup:** When the task wakes up (`enq_flags & SCX_ENQ_WAKEUP`), `grid_enqueue` consumes `meta->sleep_flag`, translates it via `sleep_flag_to_row[flag]`, assigns row $i$, and resets `meta->sleep_flag = SLEEP_FLAG_NONE`.
3. **Preemption / Expiration:** If a task stops while still runnable (preempted or timeslice expired), it retains its row assignment without reset.

### Multi-Tier Fairness Engine

To prevent task starvation and comply with eBPF's strict 30-second runnable task timeout, the scheduler enforces a dual-tier fairness mechanism:

```
[ grid_dispatch ]
       │
       ▼
 ┌───────────────┐        Yes        ┌────────────────────────┐
 │ summary == 0? ├──────────────────►│     try_work_steal     │
 └───────┬───────┘                   └────────────────────────┘
         │ No
         ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Inter-Row Fairness: Deficit Round Robin (DRR)             │
 │  Iterate rows from next_row_sched:                         │
 │    deficit[row] = min(deficit[row] + q[row], MAX_DEFICIT)  │
 └────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Intra-Row Fairness: Standard Round Robin (RR)             │
 │  Scan cols from next_col_sched:                            │
 │    Check if bit set AND deficit[row] >= cost (slices[col]) │
 └────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
                  Dispatch Task:
            scx_bpf_dsq_move_to_local()
            deficit[row] -= cost
            next_col_sched = (col + 1) % 8
            next_row_sched = (row + 1) % 8
```

#### Inter-Row Fairness: Deficit Round Robin (DRR)
* Rows $i \in [0, 7]$ are scheduled using Deficit Round Robin.
* Each non-empty row is credited with quantum $q_i$:
  $$\text{deficit}_i \leftarrow \text{deficit}_i + q_i$$
* **Deficit Bounding Rule:** In a single-task-per-dispatch CPU scheduler, deficit accumulation is capped to prevent deficit runaway and CPU hoarding:
  $$\text{deficit}_i \le q_i + t_{\max}$$
  where $t_{\max} = \text{slices}[7] = 128\text{ ms}$.
* **Scheduling Condition:** A row can schedule a task in column $j$ if $\text{deficit}_i \ge t_j$. Upon dispatch, $\text{deficit}_i \leftarrow \text{deficit}_i - t_j$.
* **Inter-Row Cursor:** On dispatch, `state->next_row_sched = (row + 1) % 8`. If all DRR iterations finish without a dispatch, `next_row_sched` is advanced to guarantee forward progress across rounds.

#### Intra-Row Fairness: Standard Round Robin (RR)
* Within a single row $i$, the scheduler performs standard Round Robin scheduling across the 8 column queues ($j=0 \dots 7$).
* The search starts from `cpu_state_row_metadata(state, row).next_col_sched`.
* Upon a successful dispatch at column $j$, the intra-row pointer is advanced:
  $$\text{next\_col\_sched} = (j + 1) \pmod 8$$

### Task Lifecycle & Preemption Rules

1. **Initial Task Placement & Wakeup Selection (`grid_select_cpu`):**
   * Triggered when a task wakes up or is newly created/forked.
   * Scans all online CPUs starting from `prev_cpu` to find the CPU with the fewest runnable tasks currently in its dispatch grid:
     $$\text{target\_cpu} = \arg\min_{c \in \text{cpus\_allowed}(p)} (\text{state}_c\to\text{total\_tasks})$$
   * **Cache Locality Tie-Breaking:** Starting the ring probe at `prev_cpu` ensures that if `prev_cpu` is tied for the minimum load (e.g. 0 tasks), the task remains on its previous CPU to preserve cache warmth.
   * **Affinity Masking:** Validates candidate CPUs against `p->cpus_ptr->bits` using `task_can_run_on_cpu`.
   * **Atomic Read:** Reads each candidate core's `total_tasks` using `READ_ONCE` to prevent compiler tearing and memory inconsistency.
2. **Deschedule Event (`grid_stopping`):**
   * Computes actual execution duration $T = \text{now} - \text{timeslice\_start}$.
   * Applies X-axis feedback to compute $c_{next}$.
   * Assigns task to current local CPU: $\text{dsq\_id} = \text{GRID\_DSQ\_ID}(\text{cpu}, \text{GRID\_DSQ\_COORD}(i_{next}, c_{next}))$.
   * If `runnable` is true, reports `EVENT_TASK_PREEMPT`.
3. **Enqueue Event (`grid_enqueue`):**
   * Resolves target CPU: reads `scx_bpf_task_cpu(p)` (the CPU selected by `grid_select_cpu`).
   * If `task_metadata->on_queue` is already true (re-enqueue without dequeue), handles state consistency via `cpu_state_deq_task` on `prev_cpu`.
   * If waking up (`SCX_ENQ_WAKEUP`), updates row $i$ based on `sleep_flag`.
   * Sets timeslice quantum: $t_c = \text{slices}[c] \times 10^6\text{ ns}$.
   * Inserts task into target CPU's DSQ via `scx_bpf_dsq_insert(p, dsq, t_c, 0)`.
   * Updates target CPU summary bitmap and task counters via `cpu_state_enq_task`.
   * **Remote Core Wakeup:** If `target_cpu != bpf_get_smp_processor_id()`, sends a kick IPI via `scx_bpf_kick_cpu(target_cpu, 0)` so the remote core immediately dispatches the newly assigned task.
   * Evaluates Wakeup Preemption for locally enqueued tasks.
4. **Wakeup Preemption Trigger:**
   * When a waking task is enqueued on the local core with target latency $l_{wake}$ and row $i_{wake}$:
   * Evaluates the currently executing task `curr` on that CPU.
   * Issues preemption (`scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT)`) if and only if:
     $$i_{wake} < i_{curr} \quad \land \quad T_{rem} > l_{wake}$$
     where $T_{rem} = t_{curr\_slice} - (\text{now} - t_{curr\_start})$.
5. **Dispatch Event (`grid_dispatch`):**
   * If `state->summary == 0`, invokes `try_work_steal`.
   * Otherwise, executes DRR inter-row selection and RR intra-row selection.
   * Moves task to CPU local queue via `scx_bpf_dsq_move_to_local(dsq)`.
   * Deducts cost from row deficit, decrements counters via `cpu_state_deq_task`, advances row and column pointers, and reports `EVENT_TASK_DISPATCH`.
6. **Running Event (`grid_running`):**
   * Records execution start timestamp: `timeslice_start = bpf_ktime_get_ns()`.
   * Clears `on_queue = false`.
   * Re-associates `dsq_id` with local processor ID.

---

## Multi-Core Load Balancing: Push Placement & Pull Stealing

The scheduler enforces a complementary dual-strategy load balancing architecture:
1. **Push Placement (`grid_select_cpu`):** Proactively places waking and newly spawned processes onto the least-loaded CPU grid, preventing initial core clustering.
2. **Pull Stealing (`try_work_steal`):** Reactively allows idle cores to steal work from busy cores when their own grids run empty.

### Full-Topology Ring Probe (Work Stealing)

* **Traversal:** The idle core probes remote cores in round-robin sequence starting from $(\text{cpu} + 1) \pmod N$ across all online cores ($N = \text{nr\_cpus}$, bounded to 64).
* **Instant Rejection of Idle Cores:** The probing core inspects the remote core's 64-bit `remote->summary` bitmap in a single memory lookup. Cores with `remote->summary == 0` are bypassed immediately without inspecting queues.
* **Remote Inter-Row Fairness:** When probing a remote core with active tasks (`remote->summary != 0`), queue scanning begins at that remote core's `next_row_sched` cursor.
* **Bounded Verifier Complexity:** To guarantee that the BPF program stays well within the kernel's 1,000,000 instruction verification limit, deep DSQ cell scanning is bounded to at most $\text{WORK\_STEAL\_MAX\_CPUS} = 4$ active remote candidate cores.
* **Affinity-Preserving Stealing:**
  * Work stealing extracts tasks using `scx_bpf_dsq_move_to_local(remote_dsq)`.
  * If the task at the head of a remote DSQ has CPU affinity restrictions that prevent execution on the stealing core, `scx_bpf_dsq_move_to_local` returns `false`.
  * The remote core's summary bit is **never** cleared upon a failed steal; only `cpu_state_deq_task` on successful removal clears bits.
* **Non-Interfering Cursor:** Stealing cores execute `cpu_state_deq_task` on the remote core to maintain precise queue counts, but do not alter the remote core's internal `next_row_sched` or DRR deficit counters.

---

## Telemetry & User-Space Observability

The scheduler incorporates a dedicated observability pipeline for telemetry, live TUI monitoring, and post-mortem analysis.

### Discrete Event Ring Buffer (`grid_events`)

An eBPF ring buffer map (`grid_events`, 256 KB) streams discrete scheduling events to user space:

| Event Type                 | Description                                      | Payload Fields                                            |
| :------------------------- | :----------------------------------------------- | :-------------------------------------------------------- |
| `EVENT_TASK_ENQUEUE`       | Task enqueued to a custom grid DSQ               | `pid`, `old_dsq`, `new_dsq`, `timestamp`                  |
| `EVENT_TASK_DISPATCH`      | Task dispatched from custom DSQ to local DSQ     | `pid`, `dsq`, `timestamp`                                 |
| `EVENT_TASK_CANCELLED`     | Task dequeued before dispatch (e.g., killed)     | `pid`, `dsq`, `timestamp`                                 |
| `EVENT_TASK_PREEMPT`       | Task preempted by a waking latency-critical task | `pid`, `dsq`, `victim_pid`, `timestamp`                   |
| `EVENT_TASK_MIGRATE`       | Task stolen from remote CPU via work stealing    | `pid`, `src_cpu`, `dst_cpu`, `dsq`, `timestamp`          |
| `EVENT_SCHED_START`        | Scheduler initialized and attached               | `timestamp`                                               |
| `EVENT_SCHED_END`          | Scheduler detached and terminated                | `timestamp`                                               |

### Interactive Terminal User Interface (TUI)

When invoked with `-t`, the user-space loader initializes an interactive `ncursesw` interface rendering real-time scheduler state:

* **Page 0 — Live Event Log:** Displays streaming telemetry events with timestamps, event types, decomposed DSQ coordinates, and PID annotations. Supports vertical scrolling.
* **Page 1 — System Overview:** Summary table across all CPUs displaying active task counts, occupancy status, and a full 64-bit binary representation of each core's `summary` bitmap.
* **Pages $2 \dots (N+1)$ — Per-CPU Grid Inspector:** Comprehensive inspection card for each individual CPU rendering:
  * Total tasks in local grid.
  * Next inter-row DRR cursor (`next_row_sched`).
  * 64-bit summary bitmap string.
  * Full $8 \times 8$ matrix grid showing individual cell queue depths (color-coded for active queues), row task totals, current row DRR deficits, and intra-row column cursors (`next_col_sched`).

### File Event Logging (`-o`)

When invoked with `-o <filename>`, every ring buffer event is written out with timestamp, event name, and decomposed DSQ parameters:
```
EVENT [38223020220736]: TASK_ENQUEUE    pid=10069 new_dsq=0x1b (cpu=0, row=3, col=3)
EVENT [38223020230811]: TASK_DISPATCH   pid=0 dsq=0x1b (cpu=0, row=3, col=3)
EVENT [38223020333870]: TASK_MIGRATE    pid=0 src_cpu=0 dst_cpu=1 dsq=0x1b (target_cpu=0, row=3, col=3)
```

---

## Implementation State Machine & Invariants

### State Machine Lifecycle

```
             ┌────────────────────────────────────────────────────────────────────────┐
             │                                                                        │
             ▼                                                                        │
      [ WAKEUP / FORK ]                                                               │
             │                                                                        │
             ▼                                                                        │
      grid_select_cpu()                                                               │
             │                                                                        │
             ▼                                                                        │
      grid_enqueue() ──► scx_bpf_dsq_insert() ──► cpu_state_enq_task() ──► [kick_cpu] │
             │                                                                        │
             ▼                                                                        │
      [ RUNNABLE IN DSQ ] ◄─────────────────┐                                         │
             │                              │                                         │
             ▼                              │                                         │
      grid_dispatch() / try_work_steal()    │                                         │
             │                              │                                         │
             ▼                              │                                         │
      scx_bpf_dsq_move_to_local()           │                                         │
      cpu_state_deq_task()                  │                                         │
             │                              │                                         │
             ▼                              │                                         │
      grid_running()                        │                                         │
             │                              │                                         │
             ▼                              │                                         │
      [ EXECUTING ON CPU ]                  │                                         │
             │                              │                                         │
             ▼                              │                                         │
      grid_stopping()                       │                                         │
             │                              │                                         │
             ├──────── (Preempted / Expired)┘                                         │
             │                                                                        │
             └──────── (Blocked / Sleeping) ──────────────────────────────────────────┘
```

### Core Invariants

1. **Bitmap Consistency:** Bit $(i \times 8 + j)$ in `state->summary` is set to `1` if and only if `task_counters[i * 8 + j] > 0`.
2. **Total Count Consistency:** For any CPU state, `total_tasks` equals the sum of all 64 elements in `task_counters`.
3. **Queue Insertion Before Visibility:** A task is always inserted into `scx_bpf_dsq_insert` before `summary` or `task_counters` indicate occupancy.
4. **Bounded DRR Deficit:** For all rows $i \in [0, 7]$, $\text{deficit}_i \in [0, q_i + 128]$. Deficit is strictly 0 whenever row $i$ has no runnable tasks.
5. **Work Stealing Safety:** A core failing to steal a task due to CPU affinity never mutates the target CPU's summary bitmap or DRR deficit.
6. **Cross-CPU Isolation & Data Race Freedom:** Remote cores performing initial placement (`grid_select_cpu`), work stealing (`try_work_steal`), or remote enqueues (`grid_enqueue`) operate strictly on atomic fields (`total_tasks`, `task_counters`, `summary`) using `READ_ONCE` and atomic RMW instructions. Remote cores never mutate or read another CPU's private DRR scheduling state (`rows`, `next_row_sched`).
