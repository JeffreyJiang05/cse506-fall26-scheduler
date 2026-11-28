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

/**
 * Worker thread parameters.
 */
typedef struct {
    int thread_id;
    int target_cpu;
    bool pin_cpu;
    size_t working_set_kb;
    uint64_t iterations;
    atomic_bool *start_signal;
    volatile uint64_t result_checksum;
} worker_arg_t;

/**
 * Display command-line usage.
 */
static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -t <threads_per_core>  Number of worker threads per online CPU core (default: 1)\n");
    fprintf(stderr, "  -T <total_threads>     Explicit total worker threads (overrides -t)\n");
    fprintf(stderr, "  -i <iterations>        Outer computation iterations per thread (default: 10000)\n");
    fprintf(stderr, "  -s <size_kb>           Working set buffer size per thread in KiB (default: 64)\n");
    fprintf(stderr, "  -p                     Pin each thread to a target CPU core (default: unpinned)\n");
    fprintf(stderr, "  -q                     Quiet mode: minimize console output\n");
    fprintf(stderr, "  -h                     Display this help message and exit\n");
}

/**
 * Worker thread routine:
 * - Allocates a cache-line aligned working set buffer (sized to fit L1/L2 cache).
 * - Avoids voluntary sleeping or yielding syscalls.
 * - Sychronizes startup using an atomic spin barrier with hardware pause hint.
 * - Executes tight, cache-hot arithmetic iterations to maximize CPU utilization.
 */
static void *worker_thread_func(void *arg)
{
    worker_arg_t *w = (worker_arg_t *)arg;

    /* Optional CPU affinity pinning */
    if (w->pin_cpu) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(w->target_cpu, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
            fprintf(stderr, "[Thread %d] Warning: failed to pin to CPU %d: %s\n",
                    w->thread_id, w->target_cpu, strerror(errno));
        }
    }

    /* Allocate 64-byte aligned working set buffer */
    size_t num_elements = (w->working_set_kb * 1024) / sizeof(uint64_t);
    if (num_elements == 0)
        num_elements = 1;

    uint64_t *buffer = aligned_alloc(64, num_elements * sizeof(uint64_t));
    if (!buffer) {
        fprintf(stderr, "[Thread %d] Error: aligned_alloc failed: %s\n",
                w->thread_id, strerror(errno));
        return NULL;
    }

    /* Initialize buffer elements */
    for (size_t i = 0; i < num_elements; i++) {
        buffer[i] = (uint64_t)(w->thread_id * 10007 + i);
    }

    /*
     * Synchronize start across all threads without voluntary sleeping.
     * Uses atomic spin-wait with hardware CPU pause instruction to avoid
     * kernel sleep system calls (futex/nanosleep).
     */
    while (!atomic_load_explicit(w->start_signal, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    }

    /*
     * Compute-intensive, cache-hot workload:
     * Continuously mutates the cache-resident working set using linear congruential
     * arithmetic, bit rotations, and XOR transformations. Runs for long timeslices
     * without voluntary context switches, maintaining L1/L2 cache warmth.
     */
    uint64_t sum = (uint64_t)(w->thread_id + 1);
    const uint64_t iters = w->iterations;

    for (uint64_t iter = 0; iter < iters; iter++) {
        for (size_t i = 0; i < num_elements; i++) {
            sum = (sum * 6364136223846793005ULL) + 1442695040888963407ULL;
            buffer[i] ^= sum;
            sum += (buffer[i] << 13) | (buffer[i] >> 51);
        }
    }

    /* Store volatile result checksum to prevent compiler dead code elimination */
    w->result_checksum = sum + buffer[0];

    free(buffer);
    return NULL;
}

int main(int argc, char *argv[])
{
    int nr_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nr_cpus <= 0)
        nr_cpus = 1;

    int threads_per_core = 1;
    int total_threads = -1;
    uint64_t iterations = 10000;
    size_t working_set_kb = 64;
    bool pin_cpu = false;
    bool quiet = false;
    int opt;

    /* Parse command-line options */
    while ((opt = getopt(argc, argv, "t:T:n:i:s:pqh")) != -1) {
        switch (opt) {
        case 't': {
            char *endptr = NULL;
            long val = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || val <= 0) {
                fprintf(stderr, "Error: Invalid threads_per_core '%s'. Must be > 0.\n", optarg);
                return 1;
            }
            threads_per_core = (int)val;
            break;
        }
        case 'T':
        case 'n': {
            char *endptr = NULL;
            long val = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || val <= 0) {
                fprintf(stderr, "Error: Invalid total_threads '%s'. Must be > 0.\n", optarg);
                return 1;
            }
            total_threads = (int)val;
            break;
        }
        case 'i': {
            char *endptr = NULL;
            unsigned long long val = strtoull(optarg, &endptr, 10);
            if (*endptr != '\0' || val == 0) {
                fprintf(stderr, "Error: Invalid iterations '%s'. Must be > 0.\n", optarg);
                return 1;
            }
            iterations = (uint64_t)val;
            break;
        }
        case 's': {
            char *endptr = NULL;
            long val = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || val <= 0) {
                fprintf(stderr, "Error: Invalid working_set_kb '%s'. Must be > 0.\n", optarg);
                return 1;
            }
            working_set_kb = (size_t)val;
            break;
        }
        case 'p':
            pin_cpu = true;
            break;
        case 'q':
            quiet = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Calculate total thread count */
    if (total_threads <= 0) {
        total_threads = nr_cpus * threads_per_core;
    }

    if (!quiet) {
        printf("====================================================\n");
        printf(" CPU-Intensive Multi-Threaded Benchmark (cpustress)\n");
        printf("====================================================\n");
        printf(" Online CPU Cores:      %d\n", nr_cpus);
        printf(" Total Worker Threads:  %d (threads/core: %.2f)\n",
               total_threads, (double)total_threads / nr_cpus);
        printf(" Working Set / Thread:  %zu KiB (%zu elements)\n",
               working_set_kb, (working_set_kb * 1024) / sizeof(uint64_t));
        printf(" Iterations / Thread:   %llu\n", (unsigned long long)iterations);
        printf(" Total Operations:      %.3e ops\n",
               (double)total_threads * iterations * ((working_set_kb * 1024) / sizeof(uint64_t)));
        printf(" CPU Affinity Pinning:  %s\n", pin_cpu ? "ENABLED" : "DISABLED (Scheduler Dynamic)");
        printf(" Voluntary Sleeping:    DISABLED (Continuous Compute)\n");
        printf("----------------------------------------------------\n");
        printf(" Launching workers...\n");
    }

    pthread_t *threads = malloc(total_threads * sizeof(pthread_t));
    worker_arg_t *args = malloc(total_threads * sizeof(worker_arg_t));
    if (!threads || !args) {
        fprintf(stderr, "Error: Failed to allocate thread tracking buffers.\n");
        return 1;
    }

    atomic_bool start_signal = ATOMIC_VAR_INIT(false);

    /* Spawn worker threads */
    for (int i = 0; i < total_threads; i++) {
        args[i].thread_id = i;
        args[i].target_cpu = i % nr_cpus;
        args[i].pin_cpu = pin_cpu;
        args[i].working_set_kb = working_set_kb;
        args[i].iterations = iterations;
        args[i].start_signal = &start_signal;
        args[i].result_checksum = 0;

        int rc = pthread_create(&threads[i], NULL, worker_thread_func, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "Error: Failed to spawn thread %d: %s\n", i, strerror(rc));
            return 1;
        }
    }

    struct timespec ts_start, ts_end;

    /* Release workers simultaneously without sleeping */
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    atomic_store_explicit(&start_signal, true, memory_order_release);

    /* Await completion of all threads */
    uint64_t total_checksum = 0;
    for (int i = 0; i < total_threads; i++) {
        pthread_join(threads[i], NULL);
        total_checksum += args[i].result_checksum;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed_sec = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                         (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    double num_elements = (double)((working_set_kb * 1024) / sizeof(uint64_t));
    double total_ops = (double)total_threads * (double)iterations * num_elements;
    double ops_per_sec = total_ops / elapsed_sec;

    if (!quiet) {
        printf("====================================================\n");
        printf(" Benchmark Execution Summary\n");
        printf("====================================================\n");
        printf(" Status:                COMPLETED\n");
        printf(" Elapsed Real Time:     %.4f seconds\n", elapsed_sec);
        printf(" Compute Throughput:    %.2f MOps/sec (%.2f GOps/sec)\n",
               ops_per_sec / 1e6, ops_per_sec / 1e9);
        printf(" Aggregate Checksum:    0x%016llx\n", (unsigned long long)total_checksum);
        printf("====================================================\n");
    } else {
        printf("threads=%d elapsed=%.4f mops=%.2f\n",
               total_threads, elapsed_sec, ops_per_sec / 1e6);
    }

    free(threads);
    free(args);
    return 0;
}
