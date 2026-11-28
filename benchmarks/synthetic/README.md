# Synthetic Benchmark: Orthogonal Latency vs. Timeslice Evaluation

## 1. Overview & Theoretical Motivation

This benchmark directly implements the evaluation workload proposed in `deliverables/proposal/proposal.tex` to evaluate how the **Grid Scheduler (`scx_grid`)** solves fundamental scheduling dilemmas present in **EEVDF (Linux default)**, **CFS**, and **traditional 1D MLFQ schedulers (e.g. FreeBSD ULE)**.

### The Problem: Conflation of Latency Requirements and Timeslices

Traditional kernel schedulers treat process behavior as a one-dimensional spectrum between **interactive** and **batch**:
- **Interactive:** Sleeps often, runs short bursts $\to$ High priority, low latency, short time slice.
- **Batch:** Long execution bursts, consumes CPU $\to$ Low priority, high latency, long time slice.

This one-dimensional coupling fails for non-traditional processes that do not neatly fit this dichotomy:

```
                      LATENCY REQUIREMENT (Y-Axis)
                 Low Latency (Fast)   High Latency (Tolerant)
               ┌─────────────────────┬─────────────────────┐
Short Bursts   │ Classic Interactive │ Sleepy Polling      │
               │ (UI events, audio)  │ (nanosleep, timers) │
TIMESLICE AXIS ├─────────────────────┼─────────────────────┤
(X-Axis)       │ Non-Traditional:    │ Classic Batch       │
Long Bursts    │ Compute-Heavy +     │ (Video encoding,    │
               │ Latency-Sensitive   │ compilation)        │
               └─────────────────────┴─────────────────────┘
```

#### Dilemma A: The Compute-Heavy + Latency-Sensitive Process
Consider a process that executes heavy mathematical or rendering computations, but coordinates with other processes via locks or synchronization events (e.g. database workers, financial trading pipelines, or game physics engines):
- **Under EEVDF / CFS:** Because the process consumes CPU, its virtual runtime ($v_i$) grows rapidly and its lag advantage disappears. When it sleeps on a `futex` and an event fires, its virtual deadline ($d_i = v_i + q_i/w_i$) is placed far into the future. It is forced to wait in the runqueue behind competing batch jobs, causing severe wakeup tail latency spikes.
- **Under Traditional MLFQ / ULE:** Because the task consumes its quantum, it is demoted to the lowest-priority batch queue. When an urgent synchronization event wakes it up, it sits behind higher-priority queues.
- **Under `scx_grid`:** Latency sensitivity and timeslice length are **orthogonal**:
  - **Y-Axis (Latency Target):** Entering `SYS_futex` flags the task with `SLEEP_FLAG_FUTEX`. On wakeup, it is placed directly into **Row 0** ($l_0 = 1\text{ ms}$, highest latency priority).
  - **Wakeup Preemption:** If a background batch job is currently running on that core, `scx_grid` evaluates $i_{\text{wake}} < i_{\text{curr}}$ and immediately preempts the batch job (`scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT)`), ensuring immediate dispatch.
  - **X-Axis (Timeslice Feedback):** Its timeslice is preserved or demoted to large quanta ($t_c \ge 16\text{ ms}$), providing cache warmth and high compute throughput.

#### Dilemma B: The Sleepy + Latency-Insensitive Process
Consider a background periodic daemon or telemetry logger that calls `nanosleep()` frequently and executes a brief operation:
- **Under Traditional MLFQ / ULE:** Because it yields before using its slice, MLFQ falsely classifies it as "interactive", elevating it to the highest priority queue and causing it to preempt long-running compute jobs.
- **Under `scx_grid`:** `nanosleep` is classified into **Row 5** (`SLEEP_FLAG_SLEEP`, $l_5 = 13\text{ ms}$), preventing it from falsely preempting normal batch or latency-critical workloads.

---

## 2. Benchmark Architecture

The synthetic benchmark models these competing workload classes under controlled CPU contention:

1. **Latency-Sensitive Compute Worker (`CLWorker`)**:
   - Blocks on a `futex` via `SYS_futex(..., FUTEX_WAIT_PRIVATE, ...)`.
   - On wakeup, measures precise scheduling delay: $\Delta t = t_{\text{wake}} - t_{\text{signal}}$.
   - Executes an intensive cache-resident arithmetic compute burst ($W$ operations).
   - Repeats for $N$ cycles, recording every latency sample.
2. **Background Batch Competitors (`BatchWorker`)**:
   - $M$ threads (default: $N_{\text{cores}}$) running continuous compute loops without voluntary sleeps, saturating all CPU cores.
   - Forces the scheduler to decide whether a waking `CLWorker` must wait in line (EEVDF) or preempt the running batch job (`scx_grid`).
3. **Sleepy Background Workers (`SleepyWorker`)**:
   - Periodically sleeps for 1 ms and performs trivial work, testing whether the scheduler avoids false interactive promotion.
4. **Event Generator**:
   - Periodically signals the `CLWorker` via `SYS_futex(..., FUTEX_WAKE_PRIVATE, ...)`, capturing $t_{\text{signal}}$ immediately before the wakeup system call.

---

## 3. Metrics Reported

- **Wakeup Latency Distribution:** Min, Mean, Median (p50), 90th (p90), 95th (p95), 99th (p99), and Max latency in microseconds ($\mu$s).
- **Jitter & Standard Deviation:** Quantifies predictability and tail latency stability.
- **Compute Throughput:** Aggregate Mega-Operations per second (MOps/sec).
- **Raw Export (`-o <file>`):** Exports all raw latency samples to a CSV file for plotting latency CDFs.

---

## 4. Command-Line Options

| Option | Argument | Default | Description |
| :--- | :--- | :--- | :--- |
| `-c` | `<workers>` | `1` | Number of compute-heavy + latency-sensitive workers. |
| `-b` | `<workers>` | `nr_cpus` | Number of background batch contention threads. |
| `-s` | `<workers>` | `0` | Number of sleepy latency-insensitive threads. |
| `-i` | `<iters>` | `500` | Number of event/evaluation cycles. |
| `-w` | `<ops>` | `20000` | Operations per compute burst for CL workers (~5–10 ms). |
| `-p` | `<ms>` | `10` | Signaling interval in milliseconds. |
| `-o` | `<file>` | none | Export raw latency samples (in microseconds) to CSV. |
| `-q` | *(flag)* | disabled | Quiet mode: prints single-line machine-readable metrics. |
| `-h` | *(flag)* | disabled | Display usage help and exit. |

---

## 5. Building and Running

### Build

```bash
# Build standalone
make -C benchmarks/synthetic

# Or build via the top-level benchmarks Makefile
make -C benchmarks synthetic/synthetic
```

### Running the Comparison: EEVDF vs. Grid Scheduler

#### Step 1: Run under Linux EEVDF (Standard Kernel Scheduler)
```bash
# Run with 1 latency-sensitive compute worker competing against 20 batch hogs
./benchmarks/synthetic/synthetic -c 1 -b 20 -i 500 -w 20000 -o eevdf_lat.csv
```

#### Step 2: Run under the Grid Scheduler (`scx_grid`)
```bash
# In terminal 1: Launch scx_grid
sudo ./bin/grid_sched

# In terminal 2: Run the identical synthetic benchmark
./benchmarks/synthetic/synthetic -c 1 -b 20 -i 500 -w 20000 -o grid_lat.csv
```

### Expected Results

| Metric | Linux EEVDF / CFS | Grid Scheduler (`scx_grid`) | Why? |
| :--- | :--- | :--- | :--- |
| **Median Latency (p50)** | $500 - 2,500\,\mu\text{s}$ | $< 50 - 150\,\mu\text{s}$ | `scx_grid` places futex wakeups in **Row 0** ($l_0 = 1\text{ms}$). |
| **Tail Latency (p99)** | $5,000 - 15,000\,\mu\text{s}$ | $< 250\,\mu\text{s}$ | EEVDF pushes deadline back due to compute; `scx_grid` enforces **Wakeup Preemption**. |
| **Compute Throughput** | High | High | `scx_grid` decouples latency from slice length, preserving large $t_c$ on X-axis. |
