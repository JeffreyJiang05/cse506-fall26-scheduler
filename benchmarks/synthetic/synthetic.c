#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <linux/futex.h>
#include <sys/syscall.h>

/**
 * Direct futex system call wrappers.
 * Invoking SYS_futex triggers sys_enter_futex tracepoint in the kernel,
 * which scx_grid classifies into SLEEP_FLAG_FUTEX (Row 0, target latency 1ms).
 */
static inline int sys_futex_wait(volatile uint32_t *uaddr, uint32_t val)
{
    return syscall(SYS_futex, uaddr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static inline int sys_futex_wake(volatile uint32_t *uaddr, int val)
{
    return syscall(SYS_futex, uaddr, FUTEX_WAKE_PRIVATE, val, NULL, NULL, 0);
}

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Benchmark configuration and shared state.
 */
typedef struct {
    int cl_workers;          /* Number of compute-heavy + latency-sensitive workers */
    int batch_workers;       /* Number of background batch competitors */
    int sleepy_workers;      /* Number of sleepy, latency-insensitive workers */
    uint64_t iterations;     /* Total event iterations per CL worker */
    uint64_t compute_ops;    /* Operations per compute burst */
    uint32_t event_period_ms;/* Event signaling period (ms) */
    char *output_file;       /* Optional file to export raw latency samples */
    bool quiet;              /* Minimize console output */

    /* Synchronization */
    atomic_bool start_barrier;
    atomic_bool stop_flag;
    volatile uint32_t futex_word;
    atomic_uint_fast64_t signal_time_ns;

    /* Metrics */
    uint64_t total_samples;
    uint64_t *latency_samples_ns;
    atomic_uint_fast64_t total_batch_ops;
    atomic_uint_fast64_t total_cl_ops;
} bench_ctx_t;

/**
 * Print command-line usage.
 */
static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "\nEvaluates orthogonal latency vs timeslice scheduling (scx_grid vs EEVDF/MLFQ).\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c <workers>   Compute-heavy + latency-sensitive workers (default: 1)\n");
    fprintf(stderr, "  -b <workers>   Background batch contention workers (default: nr_cpus)\n");
    fprintf(stderr, "  -s <workers>   Sleepy latency-insensitive workers (default: 0)\n");
    fprintf(stderr, "  -i <iters>     Number of event/evaluation cycles (default: 500)\n");
    fprintf(stderr, "  -w <ops>       Compute operations per burst for CL workers (default: 20000)\n");
    fprintf(stderr, "  -p <ms>        Signaling interval in milliseconds (default: 10)\n");
    fprintf(stderr, "  -o <file>      Export raw latency samples (in microseconds) to file\n");
    fprintf(stderr, "  -q             Quiet mode: print machine-readable metrics only\n");
    fprintf(stderr, "  -h             Display this help message and exit\n");
}

/**
 * Dense cache-resident compute kernel:
 * Multiplies and rotates elements in an aligned 64 KiB buffer, simulating
 * intensive arithmetic processing while maintaining hot cache lines.
 */
static uint64_t run_compute_burst(uint64_t *buffer, size_t num_elements, uint64_t ops, uint64_t seed)
{
    uint64_t sum = seed;
    for (uint64_t k = 0; k < ops; k++) {
        size_t idx = k % num_elements;
        sum = (sum * 6364136223846793005ULL) + 1442695040888963407ULL;
        buffer[idx] ^= sum;
        sum += (buffer[idx] << 13) | (buffer[idx] >> 51);
    }
    return sum;
}

/**
 * WORKER 1: Compute-Heavy + Latency-Sensitive Worker (The Non-Traditional Workload)
 * - Sleeps on a futex waiting for an event signal.
 * - On wakeup, records scheduling delay (t_wake - t_signal).
 * - Executes an intensive compute burst (eating significant timeslice).
 *
 * In EEVDF / traditional MLFQ: The compute burst increases virtual runtime / demotes
 * the task to batch priority. On the next event wakeup, it experiences high scheduling lag.
 *
 * In scx_grid: The futex wait assigns SLEEP_FLAG_FUTEX (Row 0, 1ms latency target).
 * On wakeup, it triggers Wakeup Preemption against batch hogs, achieving sub-millisecond
 * wakeup latency while its X-axis position retains a large timeslice quantum.
 */
static void *cl_worker_func(void *arg)
{
    bench_ctx_t *ctx = (bench_ctx_t *)arg;

    size_t num_elements = 8192; /* 64 KiB buffer */
    uint64_t *buffer = aligned_alloc(64, num_elements * sizeof(uint64_t));
    if (!buffer)
        return NULL;

    for (size_t i = 0; i < num_elements; i++)
        buffer[i] = (uint64_t)i + 1;

    /* Spin-barrier for synchronized startup */
    while (!atomic_load_explicit(&ctx->start_barrier, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }

    uint64_t seed = (uint64_t)pthread_self();
    uint64_t samples_collected = 0;
    const uint64_t max_samples = ctx->iterations;

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed) &&
           samples_collected < max_samples) {

        uint32_t val = ctx->futex_word;

        /* Block on futex: kernel sets SLEEP_FLAG_FUTEX */
        sys_futex_wait(&ctx->futex_word, val);

        uint64_t wake_ns = get_time_ns();
        uint64_t signal_ns = atomic_load_explicit(&ctx->signal_time_ns, memory_order_relaxed);

        /* Record valid wakeup latency sample */
        if (signal_ns > 0 && wake_ns >= signal_ns && samples_collected < max_samples) {
            ctx->latency_samples_ns[samples_collected++] = (wake_ns - signal_ns);
        }

        /* Heavy compute phase */
        seed = run_compute_burst(buffer, num_elements, ctx->compute_ops, seed);
        atomic_fetch_add_explicit(&ctx->total_cl_ops, ctx->compute_ops, memory_order_relaxed);
    }

    free(buffer);
    return NULL;
}

/**
 * WORKER 2: Background Batch Competitor (CPU Hog)
 * - Continuously consumes CPU without sleeping or yielding.
 * - Occupies CPU cores to create contention.
 * - In scx_grid, resides in Row 3 (default) with maximum timeslices.
 */
static void *batch_worker_func(void *arg)
{
    bench_ctx_t *ctx = (bench_ctx_t *)arg;

    size_t num_elements = 8192;
    uint64_t *buffer = aligned_alloc(64, num_elements * sizeof(uint64_t));
    if (!buffer)
        return NULL;

    for (size_t i = 0; i < num_elements; i++)
        buffer[i] = (uint64_t)i + 42;

    while (!atomic_load_explicit(&ctx->start_barrier, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }

    uint64_t seed = (uint64_t)pthread_self();
    const uint64_t chunk = 10000;

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed)) {
        seed = run_compute_burst(buffer, num_elements, chunk, seed);
        atomic_fetch_add_explicit(&ctx->total_batch_ops, chunk, memory_order_relaxed);
    }

    free(buffer);
    return NULL;
}

/**
 * WORKER 3: Sleepy, Latency-Insensitive Worker
 * - Frequently sleeps (nanosleep) and performs minimal compute.
 * - In traditional MLFQ: incorrectly elevated to interactive queue due to short runtime.
 * - In scx_grid: classified into Row 5 (SLEEP_FLAG_SLEEP), avoiding false preemption.
 */
static void *sleepy_worker_func(void *arg)
{
    bench_ctx_t *ctx = (bench_ctx_t *)arg;

    while (!atomic_load_explicit(&ctx->start_barrier, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }

    struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed)) {
        nanosleep(&sleep_time, NULL);
        /* Trivial compute (100 iterations) */
        volatile uint64_t dummy = 0;
        for (int i = 0; i < 100; i++)
            dummy += (uint64_t)i;
        (void)dummy;
    }
    return NULL;
}

/**
 * EVENT GENERATOR:
 * Periodically signals the compute-heavy + latency-sensitive workers via futex_wake.
 */
static void *event_generator_func(void *arg)
{
    bench_ctx_t *ctx = (bench_ctx_t *)arg;

    while (!atomic_load_explicit(&ctx->start_barrier, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }

    struct timespec delay;
    delay.tv_sec = ctx->event_period_ms / 1000;
    delay.tv_nsec = (ctx->event_period_ms % 1000) * 1000000ULL;

    for (uint64_t i = 0; i < ctx->iterations; i++) {
        nanosleep(&delay, NULL);
        if (atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed))
            break;

        /* Timestamp event immediately before waking workers */
        atomic_store_explicit(&ctx->signal_time_ns, get_time_ns(), memory_order_release);
        ctx->futex_word++;
        sys_futex_wake(&ctx->futex_word, ctx->cl_workers);
    }

    return NULL;
}

/**
 * Comparator for qsort to calculate percentiles.
 */
static int cmp_uint64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

int main(int argc, char *argv[])
{
    int nr_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nr_cpus <= 0)
        nr_cpus = 1;

    bench_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cl_workers = 1;
    ctx.batch_workers = nr_cpus;
    ctx.sleepy_workers = 0;
    ctx.iterations = 500;
    ctx.compute_ops = 20000;
    ctx.event_period_ms = 10;
    ctx.output_file = NULL;
    ctx.quiet = false;

    atomic_init(&ctx.start_barrier, false);
    atomic_init(&ctx.stop_flag, false);
    ctx.futex_word = 0;
    atomic_init(&ctx.signal_time_ns, 0);
    atomic_init(&ctx.total_batch_ops, 0);
    atomic_init(&ctx.total_cl_ops, 0);

    int opt;
    while ((opt = getopt(argc, argv, "c:b:s:i:w:p:o:qh")) != -1) {
        switch (opt) {
        case 'c':
            ctx.cl_workers = atoi(optarg);
            if (ctx.cl_workers <= 0) ctx.cl_workers = 1;
            break;
        case 'b':
            ctx.batch_workers = atoi(optarg);
            if (ctx.batch_workers < 0) ctx.batch_workers = 0;
            break;
        case 's':
            ctx.sleepy_workers = atoi(optarg);
            if (ctx.sleepy_workers < 0) ctx.sleepy_workers = 0;
            break;
        case 'i': {
            long long it = atoll(optarg);
            if (it > 0) ctx.iterations = (uint64_t)it;
            break;
        }
        case 'w': {
            long long ops = atoll(optarg);
            if (ops > 0) ctx.compute_ops = (uint64_t)ops;
            break;
        }
        case 'p':
            ctx.event_period_ms = (uint32_t)atoi(optarg);
            if (ctx.event_period_ms == 0) ctx.event_period_ms = 1;
            break;
        case 'o':
            ctx.output_file = strdup(optarg);
            break;
        case 'q':
            ctx.quiet = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    ctx.total_samples = ctx.iterations * ctx.cl_workers;
    ctx.latency_samples_ns = calloc(ctx.total_samples, sizeof(uint64_t));
    if (!ctx.latency_samples_ns) {
        fprintf(stderr, "Error: Failed to allocate sample buffer.\n");
        return 1;
    }

    if (!ctx.quiet) {
        printf("========================================================================\n");
        printf(" Orthogonal MLFQ Benchmark: Evaluating EEVDF / Traditional MLFQ Flaws\n");
        printf("========================================================================\n");
        printf(" Host CPU Cores:                 %d\n", nr_cpus);
        printf(" Latency-Sensitive Compute Tasks: %d (Futex sync + %llu ops/burst)\n",
               ctx.cl_workers, (unsigned long long)ctx.compute_ops);
        printf(" Background Batch Contention:     %d threads (Continuous 100%% CPU burn)\n",
               ctx.batch_workers);
        printf(" Sleepy Background Tasks:        %d threads (nanosleep 1ms)\n",
               ctx.sleepy_workers);
        printf(" Event Signaling Interval:       %u ms (%llu total cycles)\n",
               ctx.event_period_ms, (unsigned long long)ctx.iterations);
        printf(" Target Workload Property:       Compute-Heavy + Latency-Sensitive (Non-Traditional)\n");
        printf("------------------------------------------------------------------------\n");
        printf(" Spawning threads and starting benchmark...\n");
    }

    int total_threads = ctx.cl_workers + ctx.batch_workers + ctx.sleepy_workers + 1;
    pthread_t *thread_handles = malloc(total_threads * sizeof(pthread_t));
    int th_idx = 0;

    /* 1. Event generator */
    pthread_create(&thread_handles[th_idx++], NULL, event_generator_func, &ctx);

    /* 2. Latency-sensitive compute workers */
    for (int i = 0; i < ctx.cl_workers; i++)
        pthread_create(&thread_handles[th_idx++], NULL, cl_worker_func, &ctx);

    /* 3. Background batch hogs */
    for (int i = 0; i < ctx.batch_workers; i++)
        pthread_create(&thread_handles[th_idx++], NULL, batch_worker_func, &ctx);

    /* 4. Sleepy workers */
    for (int i = 0; i < ctx.sleepy_workers; i++)
        pthread_create(&thread_handles[th_idx++], NULL, sleepy_worker_func, &ctx);

    /* Release startup barrier */
    uint64_t start_time_ns = get_time_ns();
    atomic_store_explicit(&ctx.start_barrier, true, memory_order_release);

    /* Wait for generator to complete its iterations */
    pthread_join(thread_handles[0], NULL);

    /* Signal all worker threads to stop */
    atomic_store_explicit(&ctx.stop_flag, true, memory_order_release);
    ctx.futex_word++;
    sys_futex_wake(&ctx.futex_word, ctx.cl_workers);

    for (int i = 1; i < total_threads; i++) {
        pthread_join(thread_handles[i], NULL);
    }
    uint64_t end_time_ns = get_time_ns();
    double elapsed_sec = (double)(end_time_ns - start_time_ns) / 1e9;

    /* Filter valid samples */
    uint64_t valid_count = 0;
    for (uint64_t i = 0; i < ctx.total_samples; i++) {
        if (ctx.latency_samples_ns[i] > 0)
            ctx.latency_samples_ns[valid_count++] = ctx.latency_samples_ns[i];
    }

    if (valid_count == 0) {
        fprintf(stderr, "Warning: No valid latency samples captured.\n");
        return 1;
    }

    /* Sort samples to extract percentiles */
    qsort(ctx.latency_samples_ns, valid_count, sizeof(uint64_t), cmp_uint64);

    double min_us = (double)ctx.latency_samples_ns[0] / 1000.0;
    double p50_us = (double)ctx.latency_samples_ns[valid_count * 50 / 100] / 1000.0;
    double p90_us = (double)ctx.latency_samples_ns[valid_count * 90 / 100] / 1000.0;
    double p95_us = (double)ctx.latency_samples_ns[valid_count * 95 / 100] / 1000.0;
    double p99_us = (double)ctx.latency_samples_ns[valid_count * 99 / 100] / 1000.0;
    double max_us = (double)ctx.latency_samples_ns[valid_count - 1] / 1000.0;

    double sum_us = 0.0;
    for (uint64_t i = 0; i < valid_count; i++) {
        sum_us += (double)ctx.latency_samples_ns[i] / 1000.0;
    }
    double mean_us = sum_us / (double)valid_count;

    double var_sum = 0.0;
    for (uint64_t i = 0; i < valid_count; i++) {
        double diff = ((double)ctx.latency_samples_ns[i] / 1000.0) - mean_us;
        var_sum += diff * diff;
    }
    double stddev_us = sqrt(var_sum / (double)valid_count);

    uint64_t total_ops = atomic_load(&ctx.total_batch_ops) + atomic_load(&ctx.total_cl_ops);
    double throughput_mops = ((double)total_ops / elapsed_sec) / 1e6;

    /* Export raw samples if requested */
    if (ctx.output_file) {
        FILE *fp = fopen(ctx.output_file, "w");
        if (fp) {
            fprintf(fp, "# sample_idx,wakeup_latency_us\n");
            for (uint64_t i = 0; i < valid_count; i++) {
                fprintf(fp, "%llu,%.3f\n", (unsigned long long)i, (double)ctx.latency_samples_ns[i] / 1000.0);
            }
            fclose(fp);
            if (!ctx.quiet)
                printf(" Exported %llu samples to '%s'\n", (unsigned long long)valid_count, ctx.output_file);
        } else {
            fprintf(stderr, "Warning: Failed to open output file '%s': %s\n",
                    ctx.output_file, strerror(errno));
        }
    }

    if (!ctx.quiet) {
        printf("========================================================================\n");
        printf(" Benchmark Results Summary\n");
        printf("========================================================================\n");
        printf(" Total Runtime:               %.3f seconds\n", elapsed_sec);
        printf(" Valid Samples Collected:     %llu\n", (unsigned long long)valid_count);
        printf(" Aggregate Compute Work:      %.2f MOps (%.2f MOps/sec)\n",
               (double)total_ops / 1e6, throughput_mops);
        printf("------------------------------------------------------------------------\n");
        printf(" Wakeup Latency Distribution (Scheduling Delay under Contention):\n");
        printf("   Min Latency:               %10.2f us\n", min_us);
        printf("   Mean Latency:              %10.2f us (+/- %.2f us)\n", mean_us, stddev_us);
        printf("   Median (p50):              %10.2f us\n", p50_us);
        printf("   90th Percentile (p90):     %10.2f us\n", p90_us);
        printf("   95th Percentile (p95):     %10.2f us\n", p95_us);
        printf("   99th Percentile (p99):     %10.2f us\n", p99_us);
        printf("   Max Latency:               %10.2f us\n", max_us);
        printf("========================================================================\n");
    } else {
        printf("samples=%llu min=%.2f mean=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f mops=%.2f\n",
               (unsigned long long)valid_count, min_us, mean_us, p50_us, p95_us, p99_us, max_us, throughput_mops);
    }

    free(ctx.latency_samples_ns);
    free(thread_handles);
    if (ctx.output_file)
        free(ctx.output_file);

    return 0;
}
