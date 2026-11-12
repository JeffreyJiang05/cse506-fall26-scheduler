#ifndef STAT_TYPES_H
#define STAT_TYPES_H

#include "types.h"

// TODO: REMOVE
#define RECORD_EVENT_ALL

#ifdef RECORD_EVENT_ALL 
    #define RECORD_EVENT_TASK_ENQUEUE
    #define RECORD_EVENT_TASK_DISPATCH
    #define RECORD_EVENT_TASK_CANCELLED
#endif

enum sched_event_type {
    EVENT_SCHED_START,    // When the scheduler completes initialization
    EVENT_TASK_ENQUEUE,   // When a task is enqueued to a DSQ
    EVENT_TASK_DISPATCH,  // When a task is dispatched from a DSQ
    EVENT_TASK_CANCELLED, // When a task is cancelled on the DSQ 
    EVENT_SCHED_END,      // When the scheduler terminates
};

// Event explaining why
struct sched_event {
    u64 timestamp;
    enum sched_event_type type;
    union {
        #ifdef RECORD_EVENT_TASK_ENQUEUE
        struct {
            u32 pid;
            dsq_id_t old;
            dsq_id_t new;
        } EVENT_TASK_ENQUEUE;
        #endif

        #ifdef RECORD_EVENT_TASK_DISPATCH
        struct {
            u32 pid;
            dsq_id_t dsq;
        } EVENT_TASK_DISPATCH;
        #endif

        #ifdef RECORD_EVENT_TASK_CANCELLED
        struct {
            u32 pid;
            dsq_id_t dsq;
        } EVENT_TASK_CANCELLED;
        #endif

    };
};

#endif