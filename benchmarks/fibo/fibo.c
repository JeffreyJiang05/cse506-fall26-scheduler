#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

/**
 * Print usage information to stderr.
 */
static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [-b] [-s] <N>\n", prog_name);
    fprintf(stderr, "  -b        Compute Fibonacci numbers using Binet's formula (default: iterative)\n");
    fprintf(stderr, "  -s        Silent mode: suppress output to stdout (CPU-intensive)\n");
    fprintf(stderr, "  -h        Display this help message\n");
    fprintf(stderr, "  <N>       Number of Fibonacci numbers to compute (non-negative integer)\n");
}

/**
 * Compute the n-th Fibonacci number using Binet's formula:
 * F(n) = (phi^n - psi^n) / sqrt(5)
 * where phi = (1 + sqrt(5)) / 2 and psi = (1 - sqrt(5)) / 2
 */
unsigned long long fib_binet(int n)
{
    if (n <= 0)
        return 0;
    if (n == 1)
        return 1;

    long double sqrt5 = sqrtl(5.0L);
    long double phi = (1.0L + sqrt5) / 2.0L;
    long double psi = (1.0L - sqrt5) / 2.0L;

    volatile long double val = (powl(phi, n) - powl(psi, n)) / sqrt5;
    return (unsigned long long)roundl(val);
}

/**
 * Compute and optionally display the first N Fibonacci numbers using Binet's formula.
 * Uses volatile variables to prevent compiler dead code elimination when silent.
 */
static void compute_first_n_binet(long long n, bool silent)
{
    volatile unsigned long long sink = 0;

    for (long long i = 0; i < n; i++) {
        volatile unsigned long long val = fib_binet((int)i);
        sink = val;
        if (!silent)
            printf("F(%lld) = %llu\n", i, val);
    }
    (void)sink;
}

/**
 * Compute and optionally display the first N Fibonacci numbers using the iterative method.
 * Uses volatile variables to prevent compiler dead code elimination when silent.
 */
static void compute_first_n_iterative(long long n, bool silent)
{
    if (n <= 0)
        return;

    volatile unsigned long long prev2 = 0;
    volatile unsigned long long prev1 = 1;
    volatile unsigned long long val = 0;

    for (long long i = 0; i < n; i++) {
        if (i == 0) {
            val = 0;
        } else if (i == 1) {
            val = 1;
        } else {
            val = prev1 + prev2;
            prev2 = prev1;
            prev1 = val;
        }
        if (!silent)
            printf("F(%lld) = %llu\n", i, val);
    }
}

int main(int argc, char *argv[])
{
    bool use_binet = false;
    bool silent = false;
    long long n_val = -1;
    bool n_set = false;
    int opt;

    /* Check for explicit --help */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Parse command-line options */
    while ((opt = getopt(argc, argv, "bsn:h")) != -1) {
        switch (opt) {
        case 'b':
            use_binet = true;
            break;
        case 's':
            silent = true;
            break;
        case 'n': {
            char *endptr = NULL;
            errno = 0;
            long long val = strtoll(optarg, &endptr, 10);
            if (*endptr != '\0' || errno != 0 || val < 0) {
                fprintf(stderr, "Error: Invalid value for N '%s'. Must be a non-negative integer.\n", optarg);
                return 1;
            }
            n_val = val;
            n_set = true;
            break;
        }
        case 'h':
            print_usage(argv[0]);
            return 0;
        case '?':
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* If N was not specified via -n, check positional arguments */
    if (!n_set) {
        if (optind >= argc) {
            fprintf(stderr, "Error: Missing argument for N.\n");
            print_usage(argv[0]);
            return 1;
        }

        char *endptr = NULL;
        errno = 0;
        long long val = strtoll(argv[optind], &endptr, 10);
        if (*endptr != '\0' || errno != 0 || val < 0) {
            fprintf(stderr, "Error: Invalid value for N '%s'. Must be a non-negative integer.\n", argv[optind]);
            return 1;
        }
        n_val = val;
        n_set = true;
        optind++;

        if (optind < argc) {
            fprintf(stderr, "Error: Unexpected extra argument '%s'.\n", argv[optind]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Compute Fibonacci sequence */
    if (use_binet) {
        compute_first_n_binet(n_val, silent);
    } else {
        compute_first_n_iterative(n_val, silent);
    }

    return 0;
}
