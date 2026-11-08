#include <stdio.h>
#include <signal.h>
#include <assert.h>

#include <bpf/bpf.h>
#include <scx/common.h>

#include "test.skel.h"
#include "debug.h"

static const char help_msg[] = 
"A simple sched_ext scheduler.\n"
"\n"
"See the top-level comment in .bpf.c for more details.\n"
"\n"
"Usage: %s [-f] [-v]\n"
"\n"
"  -f            Use FIFO scheduling instead of weighted vtime scheduling\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

#define DISPLAY_HELP_MSG(file, prog_name) do { \
    fprintf(file, help_msg, prog_name); \
} while (0)

static volatile sig_atomic_t exit_flag = 0;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
#ifdef DEBUG
    return vprintf(stderr, format, args);
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

static void read_stats(struct test_bpf *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	assert(nr_cpus > 0);
	__u64 cnts[2][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * 2);

	for (idx = 0; idx < 2; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

struct sched_args {
    bool fifo_sched;
};

void process_args(int argc, char *argv[], struct sched_args *out) {
    __u32 opt;
    while ((opt = getopt(argc, argv, "fh")) != -1) {
        switch (opt) {
        case 'f':
            out->fifo_sched = true;
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

int main(int argc, char *argv[]) {
    info("argc = %d", argc);
    for (int i = 0; i < argc; ++i) {
        info("argv[%d] = %s", i, argv[i]);
    }
    info("Beginning program execution");

    struct test_bpf *skel = NULL;
    struct bpf_link *link = NULL;

    install_signal_handlers();
    libbpf_set_print(libbpf_print_fn);

    struct sched_args args = { 0 };
    process_args(argc, argv, &args);

    while (true) {
        info("Opening SCX skeleton");
        skel = SCX_OPS_OPEN(simple_ops, test_bpf);
        skel->rodata->fifo_sched = args.fifo_sched;

        info("Loading SCX scheduler");
        SCX_OPS_LOAD(skel, simple_ops, test_bpf, uei);

        info("Attaching SCX scheduler");
        link = SCX_OPS_ATTACH(skel, simple_ops, test_bpf);

        while (!exit_flag && !UEI_EXITED(skel, uei)) {
            __u64 stats[2];

            read_stats(skel, stats);
            printf("local=%llu global=%llu\n", stats[0], stats[1]);
            fflush(stdout);
            sleep(1);
        }

        info("Destroying attached scheduler");
        bpf_link__destroy(link);
        __u64 ecode = UEI_REPORT(skel, uei);

        info("Destroying SCX skeleton");
        test_bpf__destroy(skel);

        if (!UEI_ECODE_RESTART(ecode)) break;
    }

    info("End of program execution");
    return 0;
}