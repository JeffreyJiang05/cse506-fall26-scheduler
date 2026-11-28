# CPU-Stress: CPU-Intensive Multi-Threaded Benchmark

## 1. Overview

`cpustress` is a simple, configurable, CPU-intensive multi-threaded benchmark designed to evaluate Linux CPU schedulers (specifically `scx_grid`). It creates continuous compute-bound pressure across CPU cores with deterministic termination and zero voluntary sleeping.

Unlike I/O-bound or IPC-heavy benchmarks (such as `hackbench`) or event-multiplexing benchmarks (such as `schbench`), `cpustress` exercises the CPU scheduler's handling of **CPU-bound tasks**, **timeslice expiration**, **MLFQ priority demotion**, and **multi-core load balancing / work stealing**.

---

## 2. Key Architectural Features

### A. Long Timeslices & Hot Caches
- **Sized Working Set:** Each worker thread allocates a dedicated 64-byte aligned memory buffer (default: 64 KiB) sized to fit entirely inside modern CPU **L1 / L2 data caches**.
- **Dense Arithmetic Mutator:** Inside the inner loop, threads perform continuous arithmetic operations (linear congruential generators, bit rotations, additions, and XOR operations) directly in-place across the working set buffer.
- **Cache Warmth:** Because the working set fits in cache and is repeatedly mutated, cache lines remain hot with high L1/L2 hit rates, saturating core execution pipelines without stalling on main memory bandwidth or bus locks.

### B. Zero Voluntary Sleeping
- **No Voluntary Yielding:** Worker threads avoid voluntary context switches. There are no calls to `usleep()`, `nanosleep()`, `sched_yield()`, `poll()`, `select()`, or blocking I/O system calls during execution.
- **Spin-Wait Start Barrier:** Threads synchronize startup using an atomic memory barrier with hardware pause hints (`__builtin_ia32_pause()` on x86-64) rather than `futex`-based synchronization or sleep loops, ensuring threads transition immediately to pure computation.
- **Forced Involuntary Preemption:** Because threads never sleep voluntarily, they run for their entire scheduler timeslices until the operating system or scheduler forcibly preempts them.

### C. Configurable Core Oversubscription
- Configurable threads-per-core parameter (`-t`) automatically scales worker threads based on the number of online CPU cores (`sysconf(_SC_NPROCESSORS_ONLN)`).
- Allows benchmarking under:
  - **Balanced Load:** 1 thread per core (e.g., 20 threads on 20 cores).
  - **Oversubscription / Contention:** 2 to 4+ threads per core (e.g., 40 to 80 threads on 20 cores).
  - **Exact Thread Count:** Overriding via `-T <threads>`.

### D. Deterministic Completion Goal
- Each thread runs for a fixed number of outer computation iterations (`-i <iterations>`).
- Aggregates a volatile result checksum across all threads to prevent compiler dead code elimination at `-O2` / `-O3`.
- Automatically terminates and reports elapsed time, total operations, and throughput in MOps/sec (Million operations per second).

---

## 3. Command-Line Options

| Option | Argument | Default | Description |
| :--- | :--- | :--- | :--- |
| `-t` | `<threads_per_core>` | `1` | Number of worker threads per online CPU core. |
| `-T`, `-n` | `<total_threads>` | `nr_cpus * t` | Explicit total worker threads (overrides `-t`). |
| `-i` | `<iterations>` | `10000` | Number of outer computation iterations per worker thread. |
| `-s` | `<size_kb>` | `64` | Working set buffer size per worker thread in KiB. |
| `-p` | *(flag)* | disabled | Pin each thread to a target CPU core (`thread_id % nr_cpus`). |
| `-q` | *(flag)* | disabled | Quiet mode: prints single-line machine-readable metrics. |
| `-h` | *(flag)* | disabled | Displays help message and exits. |

---

## 4. Building and Running

### Build

```bash
# Build within this directory
make

# Or clean
make clean
```

### Usage Examples

```bash
# 1. Standard run: 1 thread per online CPU core, 10,000 iterations, 64 KiB buffer
./cpustress

# 2. 2x oversubscribed CPU stress: 2 threads per core
./cpustress -t 2

# 3. High-load stress test: 4 threads per core, 50,000 iterations
./cpustress -t 4 -i 50000

# 4. Explicit thread count (e.g., 8 threads) with 128 KiB cache working set
./cpustress -T 8 -s 128 -i 20000

# 5. Core-pinned test (locks each thread to a specific CPU core)
./cpustress -t 1 -p
```

### Sample Output

```text
====================================================
 CPU-Intensive Multi-Threaded Benchmark (cpustress)
====================================================
 Online CPU Cores:      20
 Total Worker Threads:  20 (threads/core: 1.00)
 Working Set / Thread:  64 KiB (8192 elements)
 Iterations / Thread:   10000
 Total Operations:      1.638e+09 ops
 CPU Affinity Pinning:  DISABLED (Scheduler Dynamic)
 Voluntary Sleeping:    DISABLED (Continuous Compute)
----------------------------------------------------
 Launching workers...
====================================================
 Benchmark Execution Summary
====================================================
 Status:                COMPLETED
 Elapsed Real Time:     0.5218 seconds
 Compute Throughput:    3139.73 MOps/sec (3.14 GOps/sec)
 Aggregate Checksum:    0x7291a45f9c31b8d0
====================================================
```

---

## 5. Interaction with the `scx_grid` Scheduler

When executed under `scx_grid`:
1. **X-Axis Rightward Demotion:** Because `cpustress` threads consume their full allocated quantum without blocking or yielding early ($T \ge t_c$), the scheduler's runtime feedback engine demotes them from initial column queues ($t_0 = 1\text{ ms}$) rightward toward maximum timeslices ($t_7 = 128\text{ ms}$), maximizing CPU batch throughput.
2. **Y-Axis Classification:** With zero blocking system calls (`meta->sleep_flag == SLEEP_FLAG_NONE`), threads reside in the default baseline latency row (`Row 3`), leaving low-latency rows (Rows 0–2) available for interactive tasks.
3. **Multi-Core Work Stealing:** When run without `-p`, idle cores running `try_work_steal` can detect busy cores via `summary` and migrate `cpustress` worker threads to achieve balanced core utilization.
