#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <locale.h>
#include <langinfo.h>
#include <ncurses.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <scx/common.h>

#include "config.h"

#include "grid.skel.h"
#include "debug.h"

/* ============================================================
 * Constants & user-space mirror types
 * ============================================================ */

#define NS_PER_MS 1000000ULL

/* User-space versions of DSQ decode macros from types.h / util.h */
#define US_MASK(bits)       (~0ULL >> (64ULL - (bits)))
#define US_DSQ_COL(id)      ((uint32_t)((id) & US_MASK(COL_BITS)))
#define US_DSQ_ROW(id)      ((uint32_t)(((id) >> COL_BITS) & US_MASK(ROW_BITS)))
#define US_DSQ_CPU(id)      ((uint32_t)((id) >> (ROW_BITS + COL_BITS)))
#define US_DSQ_IDX(id)      ((uint32_t)((id) & US_MASK(ROW_BITS + COL_BITS)))

#ifndef GRID_DSQ_COL
#define GRID_DSQ_COL(id)    US_DSQ_COL(id)
#define GRID_DSQ_ROW(id)    US_DSQ_ROW(id)
#define GRID_DSQ_CPU(id)    US_DSQ_CPU(id)
#define GRID_DSQ_IDX(id)    US_DSQ_IDX(id)
#endif

/* Box-drawing characters (UTF-8) */
#define BOX_H   "─"
#define BOX_V   "│"
#define BOX_TL  "┌"
#define BOX_TR  "┐"
#define BOX_BL  "└"
#define BOX_BR  "┘"
#define BOX_LT  "├"
#define BOX_RT  "┤"
#define BOX_TT  "┬"
#define BOX_BT  "┴"
#define BOX_X   "┼"

/*
 * Ensure locale is configured for UTF-8 unicode display.
 * If the environment locale is ASCII (C / POSIX / ANSI_X3.4-1968)
 * or non-UTF-8, fallback to available UTF-8 locales (e.g. C.UTF-8)
 * so box-drawing unicode characters render properly.
 */
static void init_unicode_locale(void) {
    setlocale(LC_ALL, "");

    const char *codeset = nl_langinfo(CODESET);
    if (!codeset || strcasecmp(codeset, "UTF-8") != 0) {
        const char *utf8_locales[] = {
            "C.UTF-8",
            "C.utf8",
            "en_US.UTF-8",
            "en_US.utf8",
            "UTF-8",
            NULL
        };
        for (const char **loc = utf8_locales; *loc; loc++) {
            if (setlocale(LC_ALL, *loc)) {
                setenv("LC_ALL", *loc, 1);
                setenv("LANG", *loc, 1);
                break;
            }
        }
    }
}

/*
 * User-space mirrors of kernel-side structs.
 * Must be kept in sync with include/types.h and include/stat_types.h.
 */

/* Row metadata inside cpu_state */
struct us_row_metadata {
    int32_t  deficit;
    uint32_t next_col_sched;
};

/* Per-CPU grid state — mirrors struct cpu_state */
struct us_cpu_state {
    uint64_t summary;
    uint32_t total_tasks;
    struct us_row_metadata rows[GRID_NR_ROWS];
    uint32_t task_counters[GRID_SIZE];
    uint32_t next_row_sched;
};

/* Event types — mirrors enum sched_event_type */
enum us_sched_event_type {
    US_EVENT_SCHED_START    = 0,
    US_EVENT_TASK_ENQUEUE   = 1,
    US_EVENT_TASK_DISPATCH  = 2,
    US_EVENT_TASK_CANCELLED = 3,
    US_EVENT_TASK_PREEMPT   = 4,
    US_EVENT_TASK_MIGRATE   = 5,
    US_EVENT_SCHED_END      = 6,
};

/* Full event — mirrors struct sched_event */
struct us_sched_event {
    uint64_t timestamp;
    uint32_t type;
    uint32_t _pad;
    union {
        struct { uint32_t pid; uint64_t old; uint64_t new_dsq; } enqueue;
        struct { uint32_t pid; uint64_t dsq; }                   dispatch;
        struct { uint32_t pid; uint64_t dsq; }                   cancelled;
        struct { uint32_t pid; uint64_t dsq; uint32_t victim; }  preempt;
        struct { uint32_t pid; uint32_t src_cpu; uint32_t dst_cpu; uint64_t dsq; } migrate;
    };
};

/* ============================================================
 * Help / CLI
 * ============================================================ */

static const char help_msg[] = 
"The grid scheduler: A multidimensional multilevel feedback queue scheduler.\n"
"\n"
"Usage: %s [-h] [-t] [-l config-file] [-q config-file] [-d config-file]\n"
"\n"
"  -l config-file  Configuration file for the latencies for each row of the DSQ grid\n"
"  -q config-file  Configuration file for the time slices for each column of the DSQ grid\n"
"  -d config-file  Configuration file for the DRR quanta for each row of the DSQ grid\n"
"  -t              Launch interactive ncurses TUI\n"
"  -h              Display this help and exit\n";

#define DISPLAY_HELP_MSG(file, prog_name) do { \
    fprintf(file, help_msg, prog_name); \
} while (0)

/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t exit_flag = 0;
static const __u16 default_latencies[GRID_NR_ROWS] = { 1, 2, 3, 5, 8, 13, 21, 34 };
static const __u16 default_slices[GRID_NR_COLS] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const __u16 default_drr[GRID_NR_ROWS] = { 512, 384, 256, 192, 128, 96, 64, 32 };

/* ============================================================
 * libbpf print callback
 * ============================================================ */

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
#ifdef DEBUG
    return vfprintf(stderr, format, args);
#else
    return 0;
#endif
}

/* ============================================================
 * Signal handlers
 * ============================================================ */

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

/* ============================================================
 * CLI argument parsing
 * ============================================================ */

struct sched_args {
    char *latency_config_filename;
    char *slice_config_filename;
    char *drr_config_filename;
    int   tui_mode;
};

static void process_args(int argc, char *argv[], struct sched_args *out) {
    __u32 opt;
    while ((opt = getopt(argc, argv, "l:q:d:th")) != -1) {
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
        case 't':
            out->tui_mode = 1;
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

/* ============================================================
 * Configuration file parsing
 * ============================================================ */

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

/* ============================================================
 * Ring buffer event handling
 * ============================================================ */

static const char *event_type_name(uint32_t type) {
    switch (type) {
    case US_EVENT_SCHED_START:    return "SCHED_START";
    case US_EVENT_TASK_ENQUEUE:   return "TASK_ENQUEUE";
    case US_EVENT_TASK_DISPATCH:  return "TASK_DISPATCH";
    case US_EVENT_TASK_CANCELLED: return "TASK_CANCELLED";
    case US_EVENT_TASK_PREEMPT:   return "TASK_PREEMPT";
    case US_EVENT_TASK_MIGRATE:   return "TASK_MIGRATE";
    case US_EVENT_SCHED_END:     return "SCHED_END";
    default:                      return "UNKNOWN";
    }
}

/* Format event detail with target CPU, row, and column macros */
static void format_event_detail(const struct us_sched_event *ev, char *detail, size_t detail_sz) {
    detail[0] = '\0';
    switch (ev->type) {
    case US_EVENT_TASK_ENQUEUE: {
        uint64_t new_dsq = ev->enqueue.new_dsq;
        uint64_t old_dsq = ev->enqueue.old;
        if (old_dsq) {
            snprintf(detail, detail_sz,
                     "pid=%u new_dsq=0x%llx (cpu=%u, row=%u, col=%u) prev_dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                     ev->enqueue.pid,
                     (unsigned long long)new_dsq,
                     GRID_DSQ_CPU(new_dsq), GRID_DSQ_ROW(new_dsq), GRID_DSQ_COL(new_dsq),
                     (unsigned long long)old_dsq,
                     GRID_DSQ_CPU(old_dsq), GRID_DSQ_ROW(old_dsq), GRID_DSQ_COL(old_dsq));
        } else {
            snprintf(detail, detail_sz,
                     "pid=%u new_dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                     ev->enqueue.pid,
                     (unsigned long long)new_dsq,
                     GRID_DSQ_CPU(new_dsq), GRID_DSQ_ROW(new_dsq), GRID_DSQ_COL(new_dsq));
        }
        break;
    }
    case US_EVENT_TASK_DISPATCH: {
        uint64_t dsq = ev->dispatch.dsq;
        snprintf(detail, detail_sz,
                 "pid=%u dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                 ev->dispatch.pid,
                 (unsigned long long)dsq,
                 GRID_DSQ_CPU(dsq), GRID_DSQ_ROW(dsq), GRID_DSQ_COL(dsq));
        break;
    }
    case US_EVENT_TASK_CANCELLED: {
        uint64_t dsq = ev->cancelled.dsq;
        snprintf(detail, detail_sz,
                 "pid=%u dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                 ev->cancelled.pid,
                 (unsigned long long)dsq,
                 GRID_DSQ_CPU(dsq), GRID_DSQ_ROW(dsq), GRID_DSQ_COL(dsq));
        break;
    }
    case US_EVENT_TASK_PREEMPT: {
        uint64_t dsq = ev->preempt.dsq;
        if (ev->preempt.victim) {
            snprintf(detail, detail_sz,
                     "pid=%u victim_pid=%u dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                     ev->preempt.pid, ev->preempt.victim,
                     (unsigned long long)dsq,
                     GRID_DSQ_CPU(dsq), GRID_DSQ_ROW(dsq), GRID_DSQ_COL(dsq));
        } else {
            snprintf(detail, detail_sz,
                     "pid=%u (preempt-kick) dsq=0x%llx (cpu=%u, row=%u, col=%u)",
                     ev->preempt.pid,
                     (unsigned long long)dsq,
                     GRID_DSQ_CPU(dsq), GRID_DSQ_ROW(dsq), GRID_DSQ_COL(dsq));
        }
        break;
    }
    case US_EVENT_TASK_MIGRATE: {
        uint64_t dsq = ev->migrate.dsq;
        snprintf(detail, detail_sz,
                 "pid=%u src_cpu=%u dst_cpu=%u dsq=0x%llx (target_cpu=%u, row=%u, col=%u)",
                 ev->migrate.pid, ev->migrate.src_cpu, ev->migrate.dst_cpu,
                 (unsigned long long)dsq,
                 GRID_DSQ_CPU(dsq), GRID_DSQ_ROW(dsq), GRID_DSQ_COL(dsq));
        break;
    }
    case US_EVENT_SCHED_START:
        snprintf(detail, detail_sz, "Scheduler initialized successfully");
        break;
    case US_EVENT_SCHED_END:
        snprintf(detail, detail_sz, "Scheduler terminated");
        break;
    default:
        snprintf(detail, detail_sz, "Event type %u", ev->type);
        break;
    }
}

/* Event callback for non-TUI mode */
static int handle_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(struct us_sched_event))
        return 0;

    const struct us_sched_event *ev = (const struct us_sched_event *)data;
    char detail[256];
    format_event_detail(ev, detail, sizeof(detail));

    fprintf(stderr, "EVENT [%llu]: %-15s %s\n",
            (unsigned long long)ev->timestamp,
            event_type_name(ev->type),
            detail);
    return 0;
}

/* ============================================================
 * TUI: Event log ring (circular buffer of recent events)
 * ============================================================ */

#define EVENT_LOG_MAX 1024

struct event_log_entry {
    struct us_sched_event event;
    int valid;
};

static struct {
    struct event_log_entry entries[EVENT_LOG_MAX];
    int head;    /* next write position */
    int count;   /* total entries stored (capped at EVENT_LOG_MAX) */
} event_log;

static void event_log_init(void) {
    memset(&event_log, 0, sizeof(event_log));
}

static void event_log_push(const struct us_sched_event *ev) {
    struct event_log_entry *e = &event_log.entries[event_log.head];
    memcpy(&e->event, ev, sizeof(*ev));
    e->valid = 1;
    event_log.head = (event_log.head + 1) % EVENT_LOG_MAX;
    if (event_log.count < EVENT_LOG_MAX)
        event_log.count++;
}

/* Get entry by display index (0 = most recent) */
static const struct us_sched_event *event_log_get(int idx) {
    if (idx < 0 || idx >= event_log.count)
        return NULL;
    int pos = (event_log.head - 1 - idx + EVENT_LOG_MAX) % EVENT_LOG_MAX;
    if (!event_log.entries[pos].valid)
        return NULL;
    return &event_log.entries[pos].event;
}

/* TUI ring buffer callback — pushes into the event log */
static int tui_handle_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(struct us_sched_event))
        return 0;
    event_log_push((const struct us_sched_event *)data);
    return 0;
}

/* ============================================================
 * TUI: Format a 64-bit value as binary string (MSB first)
 * ============================================================ */

static void format_binary64(uint64_t val, int nbits, char *buf, int bufsize) {
    if (nbits > 64) nbits = 64;
    if (bufsize < nbits + 1) nbits = bufsize - 1;
    for (int i = 0; i < nbits; i++) {
        buf[i] = (val >> (nbits - 1 - i)) & 1 ? '1' : '0';
    }
    buf[nbits] = '\0';
}

/* Helper to draw a horizontal box-drawing line with column segments */
static void draw_box_row(int y, int start_x, const int *col_widths, int num_cols,
                         const char *left, const char *mid, const char *right) {
    move(y, start_x);
    printw("%s", left);
    for (int c = 0; c < num_cols; c++) {
        for (int w = 0; w < col_widths[c]; w++)
            printw("%s", BOX_H);
        if (c < num_cols - 1)
            printw("%s", mid);
    }
    printw("%s", right);
}

/* ============================================================
 * TUI: Drawing functions
 * ============================================================ */

static int tui_nr_cpus = 0;         /* populated after skel load */
static int tui_total_pages = 0;
static int tui_current_page = 0;
static int tui_scroll_offset = 0;   /* for event log scrolling */
static int tui_cpu_state_fd = -1;   /* fd for grid_cpu_state map */

static void tui_draw_header(void) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);
    (void)maxy;

    attron(A_BOLD | COLOR_PAIR(1));
    mvhline(0, 0, ' ', maxx);
    mvprintw(0, 2, "Grid Scheduler TUI");

    char page_info[80];
    if (tui_current_page == 0)
        snprintf(page_info, sizeof(page_info), "[Page %d/%d: Event Log]", tui_current_page + 1, tui_total_pages);
    else if (tui_current_page == 1)
        snprintf(page_info, sizeof(page_info), "[Page %d/%d: System Overview]", tui_current_page + 1, tui_total_pages);
    else
        snprintf(page_info, sizeof(page_info), "[Page %d/%d: CPU %d Grid]", tui_current_page + 1, tui_total_pages, tui_current_page - 2);

    mvprintw(0, maxx - (int)strlen(page_info) - 2, "%s", page_info);
    attroff(A_BOLD | COLOR_PAIR(1));

    attron(COLOR_PAIR(2));
    mvhline(1, 0, ' ', maxx);
    mvprintw(1, 2, "\u2190/\u2192: Switch pages   \u2191/\u2193: Scroll Log   q: Quit");
    attroff(COLOR_PAIR(2));
}

static void tui_draw_event_log(void) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);

    int start_y = 3;
    int max_lines = maxy - start_y - 4;
    if (max_lines < 1) return;

    int detail_w = maxx - 2 - 18 - 18 - 4;
    if (detail_w < 20) detail_w = 20;

    int col_widths[3] = { 18, 18, detail_w };

    /* Clamp scroll */
    if (tui_scroll_offset > event_log.count - max_lines)
        tui_scroll_offset = event_log.count - max_lines;
    if (tui_scroll_offset < 0)
        tui_scroll_offset = 0;

    int y = start_y;

    /* Top border */
    draw_box_row(y++, 1, col_widths, 3, BOX_TL, BOX_TT, BOX_TR);

    /* Header row */
    attron(A_BOLD);
    move(y++, 1);
    printw("%s %-16s %s %-16s %s %-*.*s %s",
           BOX_V, "Timestamp (ns)",
           BOX_V, "Event",
           BOX_V, detail_w - 2, detail_w - 2, "Details",
           BOX_V);
    attroff(A_BOLD);

    /* Header divider */
    draw_box_row(y++, 1, col_widths, 3, BOX_LT, BOX_X, BOX_RT);

    /* Data rows */
    for (int i = 0; i < max_lines; i++) {
        int idx = i + tui_scroll_offset;
        const struct us_sched_event *ev = event_log_get(idx);

        move(y++, 1);
        if (ev) {
            char detail[256];
            format_event_detail(ev, detail, sizeof(detail));

            if (i % 2 == 1) attron(COLOR_PAIR(3));

            printw("%s %-16llu %s %-16s %s %-*.*s %s",
                   BOX_V, (unsigned long long)ev->timestamp,
                   BOX_V, event_type_name(ev->type),
                   BOX_V, detail_w - 2, detail_w - 2, detail,
                   BOX_V);

            if (i % 2 == 1) attroff(COLOR_PAIR(3));
        } else {
            printw("%s %-16s %s %-16s %s %-*s %s",
                   BOX_V, "",
                   BOX_V, "",
                   BOX_V, detail_w - 2, "",
                   BOX_V);
        }
    }

    /* Bottom border */
    draw_box_row(y++, 1, col_widths, 3, BOX_BL, BOX_BT, BOX_BR);

    /* Scroll indicator */
    attron(A_DIM);
    mvprintw(y, 2, "Total Events: %d \u2502 Offset: %d \u2502 Use \u2191/\u2193 to scroll log",
             event_log.count, tui_scroll_offset);
    attroff(A_DIM);
}

static void tui_draw_system_overview(void) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);
    (void)maxy;

    int y = 3;
    int col_widths[3] = { 10, 16, 66 };

    attron(A_BOLD);
    mvprintw(y++, 2, "System Overview \u2014 %d CPUs, %d\u00d7%d Grid (%d DSQs per CPU)",
             tui_nr_cpus, GRID_NR_ROWS, GRID_NR_COLS, GRID_SIZE);
    attroff(A_BOLD);
    y++;

    /* Top border */
    draw_box_row(y++, 1, col_widths, 3, BOX_TL, BOX_TT, BOX_TR);

    /* Header */
    attron(A_BOLD);
    move(y++, 1);
    printw("%s %-8s %s %-14s %s %-64s %s",
           BOX_V, "CPU",
           BOX_V, "Tasks in Grid",
           BOX_V, "64-bit Summary Bitmap (Row 7...Row 0, Col 7...Col 0)",
           BOX_V);
    attroff(A_BOLD);

    /* Divider */
    draw_box_row(y++, 1, col_widths, 3, BOX_LT, BOX_X, BOX_RT);

    uint32_t grand_total = 0;

    for (int cpu = 0; cpu < tui_nr_cpus && y < maxy - 3; cpu++) {
        struct us_cpu_state state;
        uint32_t key = cpu;

        move(y++, 1);
        if (bpf_map_lookup_elem(tui_cpu_state_fd, &key, &state) != 0) {
            printw("%s CPU %-4d %s %-14s %s %-64s %s",
                   BOX_V, cpu,
                   BOX_V, "<error>",
                   BOX_V, "<error>",
                   BOX_V);
        } else {
            char summary_bin[65];
            format_binary64(state.summary, GRID_SIZE, summary_bin, sizeof(summary_bin));

            printw("%s CPU %-4d %s ", BOX_V, cpu, BOX_V);
            if (state.total_tasks > 0) {
                attron(A_BOLD | COLOR_PAIR(4));
                printw("%-14u", state.total_tasks);
                attroff(A_BOLD | COLOR_PAIR(4));
            } else {
                attron(A_DIM);
                printw("%-14u", state.total_tasks);
                attroff(A_DIM);
            }
            printw(" %s %-64s %s", BOX_V, summary_bin, BOX_V);

            grand_total += state.total_tasks;
        }
    }

    /* Divider before total */
    draw_box_row(y++, 1, col_widths, 3, BOX_LT, BOX_X, BOX_RT);

    /* Grand total row */
    attron(A_BOLD);
    move(y++, 1);
    printw("%s %-8s %s %-14u %s %-64s %s",
           BOX_V, "Total",
           BOX_V, grand_total,
           BOX_V, "Across all CPUs",
           BOX_V);
    attroff(A_BOLD);

    /* Bottom border */
    draw_box_row(y++, 1, col_widths, 3, BOX_BL, BOX_BT, BOX_BR);
}

static void tui_draw_cpu_grid(int cpu) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);
    (void)maxy;
    (void)maxx;

    struct us_cpu_state state;
    uint32_t key = cpu;
    int y = 3;

    if (bpf_map_lookup_elem(tui_cpu_state_fd, &key, &state) != 0) {
        mvprintw(y, 2, "Failed to read state for CPU %d", cpu);
        return;
    }

    char summary_bin[65];
    format_binary64(state.summary, GRID_SIZE, summary_bin, sizeof(summary_bin));

    /* Info card: 1 column of width 85 */
    int card_width[1] = { 85 };

    draw_box_row(y++, 1, card_width, 1, BOX_TL, BOX_H, BOX_TR);

    char line1[128], line2[128], line3[128];
    snprintf(line1, sizeof(line1), "  CPU %d Dispatch Grid State", cpu);
    snprintf(line2, sizeof(line2), "  Total Tasks in Grid: %-6u    Next DRR Row to Schedule: Row %u",
             state.total_tasks, state.next_row_sched);
    snprintf(line3, sizeof(line3), "  Summary (64-bit): %s", summary_bin);

    attron(A_BOLD);
    move(y++, 1);
    printw("%s%-85s%s", BOX_V, line1, BOX_V);
    attroff(A_BOLD);

    move(y++, 1);
    printw("%s%-85s%s", BOX_V, line2, BOX_V);

    move(y++, 1);
    printw("%s%-85s%s", BOX_V, line3, BOX_V);

    draw_box_row(y++, 1, card_width, 1, BOX_BL, BOX_H, BOX_BR);

    y++;

    /* Grid table */
    /* Columns: Row(7), C0..C7(5 each = 40), Total(7), Deficit(9), NextCol(9) */
    /* Inner width: 7 + 40 + 7 + 9 + 9 = 72. Total width with 13 borders = 85 */
    int grid_col_widths[12] = { 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 9, 9 };

    /* Top border */
    draw_box_row(y++, 1, grid_col_widths, 12, BOX_TL, BOX_TT, BOX_TR);

    /* Header row */
    attron(A_BOLD);
    move(y++, 1);
    printw("%s  Row  %s  C0 %s  C1 %s  C2 %s  C3 %s  C4 %s  C5 %s  C6 %s  C7 %s Total %s Deficit %s NextCol %s",
           BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V, BOX_V);
    attroff(A_BOLD);

    /* Divider */
    draw_box_row(y++, 1, grid_col_widths, 12, BOX_LT, BOX_X, BOX_RT);

    /* Data rows 0..7 */
    for (int row = 0; row < GRID_NR_ROWS && y < maxy - 4; row++) {
        move(y++, 1);
        printw("%s Row %-2d%s", BOX_V, row, BOX_V);

        uint32_t row_total = 0;
        for (int col = 0; col < GRID_NR_COLS; col++) {
            int idx = row * GRID_NR_COLS + col;
            uint32_t count = state.task_counters[idx];
            row_total += count;

            if (count > 0) {
                attron(A_BOLD | COLOR_PAIR(4));
                printw(" %3u ", count);
                attroff(A_BOLD | COLOR_PAIR(4));
            } else {
                attron(A_DIM);
                printw("  .  ");
                attroff(A_DIM);
            }
            printw("%s", BOX_V);
        }

        printw(" %5u %s %7d %s    %u    %s",
               row_total, BOX_V,
               state.rows[row].deficit, BOX_V,
               state.rows[row].next_col_sched, BOX_V);
    }

    /* Divider after grid */
    draw_box_row(y++, 1, grid_col_widths, 12, BOX_LT, BOX_X, BOX_RT);

    /* Column totals row */
    move(y++, 1);
    attron(A_BOLD);
    printw("%s Total %s", BOX_V, BOX_V);
    attroff(A_BOLD);

    for (int col = 0; col < GRID_NR_COLS; col++) {
        uint32_t col_total = 0;
        for (int row = 0; row < GRID_NR_ROWS; row++)
            col_total += state.task_counters[row * GRID_NR_COLS + col];

        if (col_total > 0) {
            attron(A_BOLD | COLOR_PAIR(4));
            printw(" %3u ", col_total);
            attroff(A_BOLD | COLOR_PAIR(4));
        } else {
            attron(A_DIM);
            printw("  .  ");
            attroff(A_DIM);
        }
        printw("%s", BOX_V);
    }

    printw(" %5u %s         %s         %s",
           state.total_tasks, BOX_V, BOX_V, BOX_V);

    /* Bottom border */
    draw_box_row(y++, 1, grid_col_widths, 12, BOX_BL, BOX_BT, BOX_BR);

    /* Legend */
    y++;
    attron(A_DIM);
    mvprintw(y++, 2, "Legend: Rows = Latency priority (Row 0 = highest) \u2502 Columns = Time quantum (Col 0 = shortest)");
    mvprintw(y++, 2, "        . = empty DSQ \u2502 Colored numbers = runnable tasks in DSQ");
    attroff(A_DIM);
}

static void tui_draw(void) {
    erase();
    tui_draw_header();

    if (tui_current_page == 0) {
        tui_draw_event_log();
    } else if (tui_current_page == 1) {
        tui_draw_system_overview();
    } else {
        int cpu = tui_current_page - 2;
        if (cpu >= 0 && cpu < tui_nr_cpus)
            tui_draw_cpu_grid(cpu);
    }

    refresh();
}

/* ============================================================
 * TUI: Main loop
 * ============================================================ */

static void tui_run(struct grid_bpf *skel, struct ring_buffer *rb) {
    init_unicode_locale();

    /* ncurses setup */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);  /* non-blocking getch(), 100ms */

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);    /* header */
        init_pair(2, COLOR_BLACK, COLOR_CYAN);    /* nav bar */
        init_pair(3, COLOR_WHITE, -1);            /* alternating row */
        init_pair(4, COLOR_GREEN, -1);            /* non-zero task counts */
    }

    tui_nr_cpus = skel->rodata->nr_cpus;
    tui_total_pages = 2 + tui_nr_cpus;  /* event log + system overview + per-CPU */
    tui_current_page = 0;
    tui_scroll_offset = 0;
    tui_cpu_state_fd = bpf_map__fd(skel->maps.grid_cpu_state);

    event_log_init();

    while (!exit_flag && !UEI_EXITED(skel, uei)) {
        /* Drain ring buffer events */
        if (rb)
            ring_buffer__poll(rb, 0);

        tui_draw();

        int ch = getch();
        switch (ch) {
        case 'q':
        case 'Q':
            exit_flag = 1;
            break;
        case KEY_RIGHT:
            if (tui_current_page < tui_total_pages - 1) {
                tui_current_page++;
                tui_scroll_offset = 0;
            }
            break;
        case KEY_LEFT:
            if (tui_current_page > 0) {
                tui_current_page--;
                tui_scroll_offset = 0;
            }
            break;
        case KEY_DOWN:
            if (tui_current_page == 0)
                tui_scroll_offset++;
            break;
        case KEY_UP:
            if (tui_current_page == 0 && tui_scroll_offset > 0)
                tui_scroll_offset--;
            break;
        default:
            break;
        }
    }

    endwin();
}

/* ============================================================
 * Main
 * ============================================================ */

int main(int argc, char *argv[]) {
    init_unicode_locale();

    info("argc = %d", argc);
    for (int i = 0; i < argc; ++i) {
        info("argv[%d] = %s", i, argv[i]);
    }
    info("Beginning program execution");

    struct grid_bpf *skel = NULL;
    struct bpf_link *link = NULL;
    struct ring_buffer *rb = NULL;

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

        /*
         * Set up the ring buffer for telemetry event streaming.
         */
        if (args.tui_mode) {
            rb = ring_buffer__new(bpf_map__fd(skel->maps.grid_events),
                                 tui_handle_event, NULL, NULL);
        } else {
            rb = ring_buffer__new(bpf_map__fd(skel->maps.grid_events),
                                 handle_event, NULL, NULL);
        }
        if (!rb) {
            warn("Failed to create ring buffer — events will not be streamed");
        }

        info("Attaching SCX scheduler");
        link = SCX_OPS_ATTACH(skel, grid_ops, grid_bpf);

        if (args.tui_mode) {
            tui_run(skel, rb);
        } else {
            while (!exit_flag && !UEI_EXITED(skel, uei)) {
                /* Poll the ring buffer for events (100ms timeout) */
                if (rb)
                    ring_buffer__poll(rb, 100);
                else
                    sleep(1);
            }
        }

        info("Destroying attached scheduler");
        bpf_link__destroy(link);
        __u64 ecode = UEI_REPORT(skel, uei);

        if (rb) {
            ring_buffer__free(rb);
            rb = NULL;
        }

        info("Destroying SCX skeleton");
        grid_bpf__destroy(skel);

        if (!UEI_ECODE_RESTART(ecode)) break;
    }

    info("End of program execution");
    return 0;
}
