#ifndef STATS_BPF_H
#define STATS_BPF_H

#include <scx/common.bpf.h>
#include "stat_types.h"

/*
 * Ring buffer for streaming scheduling events to user-space.
 * Size: 256 KB (must be a power of 2).
 */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} grid_events SEC(".maps");

static void submit_to_queue(struct sched_event *event) {
    struct sched_event *ev;
    ev = bpf_ringbuf_reserve(&grid_events, sizeof(*ev), 0);
    if (!ev)
        return;
    __builtin_memcpy(ev, event, sizeof(*ev));
    bpf_ringbuf_submit(ev, 0);
}

#ifdef RECORD_EVENT_TASK_ENQUEUE
    #define report_event_EVENT_TASK_ENQUEUE(_, ...) do {    \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_ENQUEUE,                     \
            .EVENT_TASK_ENQUEUE = { __VA_ARGS__ }           \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_ENQUEUE(...)
#endif

#ifdef RECORD_EVENT_TASK_DISPATCH
    #define report_event_EVENT_TASK_DISPATCH(_, ...) do {   \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_DISPATCH,                    \
            .EVENT_TASK_DISPATCH = { __VA_ARGS__ }          \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_DISPATCH(...)
#endif

#ifdef RECORD_EVENT_TASK_CANCELLED
    #define report_event_EVENT_TASK_CANCELLED(_, ...) do {  \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_CANCELLED,                   \
            .EVENT_TASK_CANCELLED = { __VA_ARGS__ }         \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_CANCELLED(...)
#endif

#ifdef RECORD_EVENT_TASK_PREEMPT
    #define report_event_EVENT_TASK_PREEMPT(_, ...) do {    \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_PREEMPT,                     \
            .EVENT_TASK_PREEMPT = { __VA_ARGS__ }           \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_PREEMPT(...)
#endif

#ifdef RECORD_EVENT_TASK_MIGRATE
    #define report_event_EVENT_TASK_MIGRATE(_, ...) do {    \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_TASK_MIGRATE,                     \
            .EVENT_TASK_MIGRATE = { __VA_ARGS__ }           \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_TASK_MIGRATE(...)
#endif

#ifdef RECORD_EVENT_SCHED_START
    #define report_event_EVENT_SCHED_START(_, ...) do {     \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_SCHED_START,                      \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_SCHED_START(...)
#endif

#ifdef RECORD_EVENT_SCHED_END
    #define report_event_EVENT_SCHED_END(_, ...) do {       \
        struct sched_event __ev = {                         \
            .timestamp = bpf_ktime_get_ns(),                \
            .type = EVENT_SCHED_END,                        \
        };                                                  \
        submit_to_queue(&__ev);                             \
    } while (0)
#else
    #define report_event_EVENT_SCHED_END(...)
#endif

#define report_event(event, ...) CAT(report_event_, event)(event, ## __VA_ARGS__)

#endif
