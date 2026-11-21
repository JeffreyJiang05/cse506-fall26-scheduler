# Grid: A Multi-Dimensional MLFQ Scheduler — Design Specification

## System Overview

The Grid Scheduler is a Linux CPU scheduler built on eBPF and the `sched_ext` (`scx`) framework. It evolves traditional Multilevel Feedback Queue (MLFQ) scheduling by decoupling execution time slice length from process latency requirements into an orthogonal, multi-dimensional dispatch grid.

## Grid Geometry & Data Structures

### Two-Dimensional Dispatch Queue Matrix

Each CPU maintains an $8 \times 8$ matrix consisting of 64 discrete eBPF Dispatch Queues (DSQs). A queue position at row index $i \in [0, 7]$ and column index $j \in [0, 7]$ binds tasks to latency requirement $l_i$ and time slice duration $t_j$.

| Dimension   | Axis   | Index Range    | Managed Property            | Policy Engine                          |
| :---------- | :----- | :------------- | :-------------------------- | :------------------------------------- |
| **Columns** | X-Axis | $j \in [0, 7]$ | Time Slice Duration ($t_j$) | Execution Time Feedback ($T$ vs $t_c$) |
| **Rows**    | Y-Axis | $i \in [0, 7]$ | Latency Target ($l_i$)      | Syscall Tracepoint Flag Analysis       |

### Grid State Summary Bitmap

To eliminate non-deterministic queue iteration within eBPF scheduling hot-paths, the active state of all 64 queues is aggregated into a single 64-bit unsigned integer bitmap (`u64 summary`).

*   **Bit Encoding:** Each bit $k \in [0, 63]$ in `summary` corresponds to queue cell $(i, j)$ where $k = i \times 8 + j$.
*   **Bit Value:** A bit set to `1` indicates that queue $(i, j)$ contains at least one runnable task; a bit set to `0` indicates an empty queue.

## User-Space Configuration Interface

The user-space launching application, built with `libbpf` and `scx` helper libraries, configures kernel-side scheduling parameters at startup via eBPF maps.

### Configuration Vectors

The scheduler exposes three key configuration vectors across the 8 rows and 8 columns passed via user-space CLI arguments:

| Parameter Vector          | Symbol | Dimensions                    | Description & Unit                                        |
| :------------------------ | :----- | :---------------------------- | :-------------------------------------------------------- |
| **Target Latency Vector** | $l_i$  | Row Array ($i \in [0, 7]$)    | Maximum latency threshold assigned to row $i$             |
| **Time Slice Vector**     | $t_j$  | Column Array ($j \in [0, 7]$) | Allotted CPU execution quantum assigned to column $j$     |
| **DRR Quantum Vector**    | $q_i$  | Row Array ($i \in [0, 7]$)    | Deficit Round Robin byte/tick quantum assigned to row $i$ |

## Core Scheduling & Dispatch Policies

### Time Slice Length Policy (X-Axis Feedback)

When a task completes an execution turn on a CPU, the scheduler evaluates its actual execution time $T$ against its assigned time slice $t_c$ at current column $c$. The column index for the task's next runnable placement is computed as follows:

1.  **Demotion (Rightward Shift):** If $T \ge t_c$, the task consumed its full quantum (CPU-bound). The next column assignment is:
    $$c_{next} = \min(c + 1, \text{NR\_COLS} - 1)$$
    where $\text{NR\_COLS} = 8$.
2.  **Promotion (Leftward Shift):** If $T < \frac{t_c}{2}$, the task yielded prematurely (interactive / I/O-bound). The next column assignment is:
    $$c_{next} = \max(c - 1, 0)$$
3.  **Retention (No Shift):** If $\frac{t_c}{2} \le T < t_c$, the task remains at column $c$:
    $$c_{next} = c$$

### Latency Requirement Policy (Y-Axis Classification)

The row placement $i$ is determined dynamically by inspecting blocking events via eBPF system call handlers.

*   **Syscall Interception:** eBPF handlers are attached to sleep-inducing system calls (such as `sys_futex`, `sys_read`, `sys_write`, and event polling).
*   **Flag Ingestion:** When a task enters a sleep state, the corresponding syscall handler writes a classification flag to the task's metadata struct describing the syscall type.
*   **Row Mapping:** Upon task wakeup, the scheduler reads this flag to compute the assigned row index $i$, mapping critical synchronization and interactive I/O syscalls to lower row indices (higher latency priority).

### Task Lifecycle & Preemption Rules

1.  **Preemption Compute Phase:** When a running task is descheduled or preempted, its updated grid indices $(i_{next}, c_{next})$ are computed and saved in the task metadata structure.
2.  **Enqueue Phase:** When the task transitions back to the runnable state, it is inserted into DSQ $(i_{next}, c_{next})$ belonging to its assigned CPU.
3.  **Row Priority Dispatching:** When selecting a task to run, the scheduler scans row indices starting from $i=0$ (highest latency priority) down to $i=7$.
4.  **Wakeup Preemption Trigger:** When a sleeping task wakes up with target latency $l_{wake}$, the scheduler compares $l_{wake}$ against the remaining execution time slice $T_{rem}$ of the currently running task on that CPU:
    $$\text{If } T_{rem} > l_{wake} \implies \text{Preempt Running Task}$$

### Multi-Tier Fairness Engine

To prevent task starvation and comply with eBPF's strict 30-second runnable task timeout, the scheduler enforces a dual-tier fairness mechanism:

*   **Inter-Row Fairness (Deficit Round Robin):** Deficit Round Robin (DRR) is executed across grid rows ($i=0 \dots 7$). Each row $i$ is allocated a deficit counter replenished by quantum $q_i$, ensuring lower-priority rows receive guaranteed CPU shares despite high-priority load.
*   **Intra-Row Fairness (Standard Round Robin):** Within a single row $i$, the scheduler performs standard Round Robin scheduling across the 8 column queues ($j=0 \dots 7$).

---

## Multi-Core Load Balancing & Work Stealing

1.  **Task Load Equalization:** The load balancer distributes tasks across cores based on total core queue depth.
2.  **Shared Memory Migration:** Grid metadata must be stored in global eBPF maps rather than strictly per-CPU maps to enable cross-CPU visibility.
3.  **Work Stealing:** If a CPU's local grid matrix becomes completely empty (`summary == 0`), it actively steals tasks from high-load grid queues on remote CPUs.

---

## Telemetry & User-Space Observability

The scheduler incorporates a dedicated observability pipeline for telemetry and analysis:

*   **State Telemetry Map:** An eBPF array map tracks per-CPU and per-DSQ task counts for live user-space polling.
*   **Ring Buffer Event Channel:** A circular event queue streams discrete scheduling events (task enqueue, dispatch, preemption, queue migration) to user-space log parsers.

---

## Implementation State Machine & Known Pitfalls

### State Machine Lifecycle Steps

1.  **Deschedule Event:**
    *   Measure actual execution time $T$.
    *   Compute next column $c_{next} = \min(c+1, 7)$ if $T \ge t_c$, or $c_{next} = \max(c-1, 0)$ if $T < t_c / 2$.
    *   Inspect system call flag set by eBPF handlers to select row $i_{next}$.
    *   Store tuple $(i_{next}, c_{next})$ in task metadata.

2.  **Enqueue Event:**
    *   Retrieve target tuple $(i_{next}, c_{next})$.
    *   Insert task into eBPF DSQ at cell $(i_{next}, c_{next})$.
    *   Set bit $(i_{next} \times 8 + c_{next})$ in the CPU summary bitmap.
    *   If current CPU running task has $T_{rem} > l_{i_{next}}$, issue a preemption request.

3.  **Dispatch Event:**
    *   Select non-empty row $i$ using Deficit Round Robin with row quantum $q_i$.
    *   Select target column $j$ within row $i$ using intra-row Round Robin.
    *   Dequeue task from DSQ $(i, j)$.
    *   If DSQ $(i, j)$ becomes empty after dequeue, clear bit $(i \times 8 + j)$ in the CPU summary bitmap.

