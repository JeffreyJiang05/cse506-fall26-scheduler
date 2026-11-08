#include <stdio.h>
#include <signal.h>

#include <bpf/bpf.h>
#include <scx/common.h>

#include "grid.skel.h"
#include "debug.h"

static const char help_msg[] = 
"The grid scheduler: A multidimensional multilevel feedback queue scheduler.\n"
"\n"
"Usage: %s\n";

#define DISPLAY_HELP_MSG(file, prog_name) do { \
    fprintf(file, help_msg, prog_name); \
} while (0)

static volatile sig_atomic_t exit_flag = 0;

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

struct sched_args {};

void process_args(int argc, char *argv[], struct sched_args *out) {
    __u32 opt;
    while ((opt = getopt(argc, argv, "h")) != -1) {
        switch (opt) {
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

    struct grid_bpf *skel = NULL;
    struct bpf_link *link = NULL;

    install_signal_handlers();
    libbpf_set_print(libbpf_print_fn);

    struct sched_args args = {};
    process_args(argc, argv, &args);

    while (true) {
        info("Opening SCX skeleton");
        skel = SCX_OPS_OPEN(grid_ops, grid_bpf);

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