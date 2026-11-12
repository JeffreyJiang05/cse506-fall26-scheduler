#ifndef STATS_BPF_H
#define STATS_BPF_H

#include <scx/common.bpf.h>
#include "stat_types.h"

static void submit_to_queue(struct sched_event *event) {

}

#ifdef RECORD_EVENT_TASK_ENQUEUE
    #define report_event_EVENT_TASK_ENQUEUE(_, ...) do {    \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_ENQUEUE,                     \
            .EVENT_TASK_ENQUEUE = { ##__VA_ARGS__ }         \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_ENQUEUE(...)
#endif

#define report_event(event, ...) CAT(report_event_, event)(event, ## __VA_ARGS__)

#endif