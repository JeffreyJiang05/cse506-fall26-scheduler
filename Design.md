# Design

## Sources 

Borrowed Virtual Time (BVT) Scheduling (SOSP 1999)

Earliest Eligible Virtual Deadline First (EEVDF)

Decoupled Delay and Rate Guarantees for CPU Scheduling

## Motivation

Scheduler design has to balance a variety of factors about processes 
in scheduling decisions. Processes being scheduled have two main 
properties:

(1) **Latency sensitivity**: Some processes should be scheduled as soon
    as possible when some event occurs

(2) **Ideal CPU time slice**: Some processes would ideally spend more
    time executing on a logical core before context switching

Traditionally, schedulers such as ULE, divided processes into two
categories: interactive processes and batch processes. Interactive
processes are latency-sensitive and executes quickly before sleeping 
while batch processes are latency-insensitive and should be given
a larger quantum. This dichotomy ignores processes that are
latency-sensitive but would like bursts of CPU execution time. For 
example, consider an LLM server which has to respond quickly to
requests, but to handle those requests would ideally run continuously.

ULE uses this dichotomy to place processes within different queues: 
one queue for interactive processes and another for batch processes. 
The interactive processes took precedence over batch processes helping 
ensure latency-sensitivity.

In CFS, virtual runtime is used to determine which process is scheduled 
first. This policy favors processes which sleep often, typically waiting 
on some event, so that when the event arrives, the schedulers can 
preempt the currently executing process with newly awoken process. This 
helps with latency-sensitivity.

However, the quanta computed by CFS are reliant on the nice values of the 
processes currently in the run queue which has to be set by the program
or the user. 

## Design

Treat latency requirements and ideal CPU quanta for a process as
orthogonal to each other. Treat each as separate axes of a grid.
Each cell of a grid represent processes which has those specific
properties. Using this information, better scheduling decisions
can be made.

Formally, adapt the Multilevel Feedback Queue (MLFQ) design and 
extend it into two dimensions: one dimension for latency requirement
and a second for quanta. Each cell within this grid is its own
queue. Processes can migrate between cells dynamically by tracking 
certain statistics.

```
 ^ +----+----+----+----+----+----+----+----+
 ^ |[00]|[01]|[02]|[03]|[04]|[05]|[06]|[07]|  0 ms
 L +----+----+----+----+----+----+----+----+
 A |[10]|[11]|[12]|[13]|[14]|[15]|[16]|[17]|  1 ms
 T +----+----+----+----+----+----+----+----+
 E |[20]|[21]|[22]|[23]|[24]|[25]|[26]|[27]|  2 ms
 N +----+----+----+----+----+----+----+----+
 C |[30]|[31]|[32]|[33]|[34]|[35]|[36]|[37]|  3 ms
 Y +----+----+----+----+----+----+----+----+
 ^ |[40]|[41]|[42]|[43]|[44]|[45]|[46]|[47]|  5 ms
 ^ +----+----+----+----+----+----+----+----+
 R |[50]|[51]|[52]|[53]|[54]|[55]|[56]|[57]|  8 ms
 E +----+----+----+----+----+----+----+----+
 Q |[60]|[61]|[62]|[63]|[64]|[65]|[66]|[67]|  13 ms
 S +----+----+----+----+----+----+----+----+
 ^ |[70]|[71]|[72]|[73]|[74]|[75]|[76]|[77]|  21 ms
 ^ +----+----+----+----+----+----+----+----+
   > > > > > Q > U > A > N > T > A > > > > >
    1ms  2ms  4ms  8ms  16ms 32ms 64ms 128ms

[00] --> [00]           (Empty Queue)
[00] --> [AA] --> [00]  (Queue with process queued)
...
[45] --> [BB] --> [CC] --> [45]
...
[77] --> [77]
```

- To track quanta: Goal is to find the ideal quanta. 
    - Schedule process on a quanta tier. If a process successfully uses
      up its quanta without waiting, move it to the next tier
    - If the process yields within half of the quantum, then schedule
      the process in a lower quantum queue

- To track latency requirements, use multiple heuristics
    - 


Deficit Round Robin

  