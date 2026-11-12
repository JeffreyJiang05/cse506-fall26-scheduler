#include <stdio.h>
#include <signal.h>

#include <bpf/bpf.h>
#include <scx/common.h>

#include "config.h"

#include "grid.skel.h"
#include "debug.h"

static const char help_msg[] = 
"The grid scheduler: A multidimensional multilevel feedback queue scheduler.\n"
"\n"
"Usage: %s [-h] [-l config-file] [-q config-file]\n"
"\n"
"  -l config-file  Configuration file for the latencies for each row of the DSQ grid\n"
"  -q config-file  Configuration file for the time slices for each column of the DSQ grid\n"
"  -h              Display this help and exit\n";

#define DISPLAY_HELP_MSG(file, prog_name) do { \
    fprintf(file, help_msg, prog_name); \
} while (0)

static volatile sig_atomic_t exit_flag = 0;
static const __u16 default_latencies[GRID_NR_ROWS] = { 1, 2, 3, 5, 8, 13, 21, 34 };
static const __u16 default_slices[GRID_NR_COLS] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const __u16 default_drr[GRID_NR_ROWS] = { 128, 128, 128, 128, 128, 128, 128, 128 };

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
#ifdef DEBUG
    return vfprintf(stderr, format, args);
#else
    return 0;
#endif
}

static void handle_exit_signal(int signum) {
    exit_flag = 1;
}

static void install_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_exit_signal;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "Failed to install SIGINT handler. Exiting..");
        exit(1);
    }
    info("Successfully installed SIGINT signal handler");

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "Failed to install SIGTERM handler. Exiting...");
        exit(1);
    }
    info("Successfully installed SIGTERM signal handler");
}

struct sched_args {
    char *latency_config_filename;
    char *slice_config_filename;
    char *drr_config_filename;
};

static void process_args(int argc, char *argv[], struct sched_args *out) {
    __u32 opt;
    while ((opt = getopt(argc, argv, "l:q:d:h")) != -1) {
        switch (opt) {
        case 'l':
            out->latency_config_filename = optarg;
            info("Latency config input file: %s", out->latency_config_filename);
            break;
        case 'q':
            out->slice_config_filename = optarg;
            info("Time slice config input file: %s", out->slice_config_filename);
            break;
        case 'd':
            out->drr_config_filename = optarg;
            info("DRR quanta config input file: %s", out->drr_config_filename);
            break;
        case 'h':
            DISPLAY_HELP_MSG(stdout, argv[0]);
            exit(0);
        default:
            DISPLAY_HELP_MSG(stderr, argv[0]);
            exit(1);
        }
    }    
}

static int parse_u16(char *token, __u16 *value) {
    int old_errno = errno;
    int ret = -1;

    errno = 0; 
    char *end; 
    long val = strtol((token), &end, 10); 
    if (errno == 0 && end != token && val >= 0 && val <= UINT16_MAX) {
        *value = (__u16) val;
        ret = 0;
    }

    errno = old_errno;
    return ret;
}

static int parse_file(FILE *file, int nr_args, char *delimiter, __u16 *output) {
    char *lineptr = NULL;
    size_t n = 0;
    ssize_t bytes_read = 0;

    bytes_read = getline(&lineptr, &n, file);
    if (!bytes_read) return -1;

    char *iter = lineptr;
    char *token = NULL; 
    int nr_tokens = 0;

    while (nr_tokens < nr_args && (token = strsep(&iter, delimiter)) != NULL) {
        if (parse_u16(token, &output[nr_tokens]) == -1) {
            warn("Failed to parse %s into u16", token);
            free(lineptr);
            return -1;
        }
        nr_tokens++;
    }

    if (nr_tokens != nr_args) {
        warn("Tokens parsed != Expected number of tokens");
        free(lineptr);
        return -1;
    }

    info("Successfully parsed file");
    free(lineptr);
    return 0;
}

static void configure_latency(struct grid_bpf *skel, char *latency_file) {
    if (!latency_file) {
        info("No supplied latency configuration file. Using default");
        memcpy(&skel->rodata->latency[0], &default_latencies[0], sizeof(default_latencies[0]) * GRID_NR_ROWS);
        return;
    }

    FILE *file = fopen(latency_file, "r");
    if (!file || parse_file(file, GRID_NR_ROWS, ":", &skel->rodata->latency[0]) == -1) {
        warn("Problem handling %s (either problem with the file or file format). Resorting to default", latency_file);
        memcpy(&skel->rodata->latency[0], &default_latencies[0], sizeof(default_latencies[0]) * GRID_NR_ROWS);
    }
    if (file) fclose(file);
}

static void configure_timeslices(struct grid_bpf *skel, char *slices_file) {
    if (!slices_file) {
        info("No supplied timeslices configuration file. Using default");
        memcpy(&skel->rodata->slices[0], &default_slices[0], sizeof(default_slices[0]) * GRID_NR_COLS);
        return;
    }

    FILE *file = fopen(slices_file, "r");
    if (!file || parse_file(file, GRID_NR_COLS, ":", &skel->rodata->slices[0]) == -1) {
        warn("Problem handling %s (either problem with the file or file format). Resorting to default", slices_file);
        memcpy(&skel->rodata->slices[0], &default_slices[0], sizeof(default_slices[0]) * GRID_NR_COLS);
    }
    if (file) fclose(file);
}

static void configure_drr_quanta(struct grid_bpf *skel, char *drr_file) {
    if (!drr_file) {
        info("No supplied DRR quanta configuration file. Using default");
        memcpy(&skel->rodata->drr_quanta[0], &default_drr[0], sizeof(default_drr[0]) * GRID_NR_ROWS);
        return;
    }

    FILE *file = fopen(drr_file, "r");
    if (!file || parse_file(file, GRID_NR_ROWS, ":", &skel->rodata->drr_quanta[0]) == -1) {
        warn("Problem handling %s (either problem with the file or file format). Resorting to default", drr_file);
        memcpy(&skel->rodata->drr_quanta[0], &default_drr[0], sizeof(default_drr[0]) * GRID_NR_ROWS);
    }
    if (file) fclose(file);
}

static void configure_scheduler(struct grid_bpf *skel, struct sched_args *args) {
    skel->rodata->nr_cpus = libbpf_num_possible_cpus();
    configure_latency(skel, args->latency_config_filename);
    configure_timeslices(skel, args->slice_config_filename);
    configure_drr_quanta(skel, args->drr_config_filename);
    

    info(" === SCHEDULER CONFIGURATION BEGIN ===");
    info(" NR_CPUS = %u", skel->rodata->nr_cpus);
    for (int i = 0; i < GRID_NR_ROWS; ++i) info(" LATENCY[%d] = %u", i, skel->rodata->latency[i]);
    for (int i = 0; i < GRID_NR_COLS; ++i) info(" SLICE[%d] = %u", i, skel->rodata->slices[i]);
    for (int i = 0; i < GRID_NR_ROWS; ++i) info(" DRR_QUANTUM[%d] = %u", i, skel->rodata->drr_quanta[i]);
    info(" === SCHEDULER CONFIGURATION END ===");
}

int main(int argc, char *argv[]) {
    info("argc = %d", argc);
    for (int i = 0; i < argc; ++i) {
        info("argv[%d] = %s", i, argv[i]);
    }
    info("Beginning program execution");

    struct grid_bpf *skel = NULL;
    struct bpf_link *link = NULL;

    install_signal_handlers();
    libbpf_set_print(libbpf_print_fn);

    struct sched_args args = { 0 };
    process_args(argc, argv, &args);

    while (true) {
        info("Opening SCX skeleton");
        skel = SCX_OPS_OPEN(grid_ops, grid_bpf);

        skel->rodata->nr_cpus = libbpf_num_possible_cpus();
        configure_scheduler(skel, &args);

        info("Loading SCX scheduler");
        SCX_OPS_LOAD(skel, grid_ops, grid_bpf, uei);

        info("Attaching SCX scheduler");
        link = SCX_OPS_ATTACH(skel, grid_ops, grid_bpf);

        while (!exit_flag && !UEI_EXITED(skel, uei)) {
            sleep(1);
        }

        info("Destroying attached scheduler");
        bpf_link__destroy(link);
        __u64 ecode = UEI_REPORT(skel, uei);

        info("Destroying SCX skeleton");
        grid_bpf__destroy(skel);

        if (!UEI_ECODE_RESTART(ecode)) break;
    }

    info("End of program execution");
    return 0;
}