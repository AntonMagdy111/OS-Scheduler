/*
 * sched_core.h
 * The scheduling engine. It is a deterministic, tick-driven state machine:
 * the caller feeds it one clock second at a time together with the processes
 * that arrive at that second. The engine decides what runs, writes
 * scheduler.log and collects statistics for scheduler.perf.
 *
 * All OS work (fork, SIGSTOP/SIGCONT, shared memory, waiting for the
 * termination message) is delegated to hooks, so the same engine is used by
 * the real IPC scheduler (scheduler.c) and by the offline simulator (sim.c).
 */
#ifndef SCHED_CORE_H
#define SCHED_CORE_H

#include <stdio.h>

#include "data_structures.h"
#include "pcb.h"
#include "process_info.h"
#include "resource.h"

typedef enum {
    ALGO_SJF = 1,      /* Shortest Job First (non-preemptive)              */
    ALGO_RR = 2,       /* Round Robin                                      */
    ALGO_HPF = 3,      /* Preemptive HPF with priority inheritance         */
    ALGO_HPF_AGING = 4 /* Bonus: HPF with priority aging and demotion      */
} Algorithm;

/* OS-level callbacks. Any of them may be NULL. */
typedef struct {
    void (*onStart)(PCB *pcb, void *context);   /* first dispatch: fork it      */
    void (*onResume)(PCB *pcb, void *context);  /* later dispatch: SIGCONT      */
    void (*onStop)(PCB *pcb, void *context);    /* preempted/blocked: SIGSTOP   */
    void (*onExecute)(PCB *pcb, void *context); /* executed one more second     */
    void (*onFinish)(PCB *pcb, void *context);  /* remaining = 0: await notice  */
    void *context;
} SchedulerHooks;

typedef struct {
    /* configuration */
    Algorithm algorithm;
    int quantum;           /* RR only                                   */
    int contextSwitchTime; /* 1 for SJF/HPF, user input for RR          */
    int usesResources;     /* resources.txt is used by HPF only         */
    FILE *logFile;
    SchedulerHooks hooks;

    /* CPU state */
    int currentTime;
    PCB *running;
    int quantumEnd;   /* RR: time at which the running quantum expires */
    int switching;    /* 1 while a context switch is in progress       */
    int switchEnd;    /* time the context switch completes             */
    int cpuReleasedAt;/* last time a process left the CPU              */

    /* ready structures */
    MinHeap readyHeap;    /* SJF / HPF */
    FifoQueue readyQueue; /* RR        */

    ResourceTable resources;

    /* all processes that arrived and did not finish yet */
    PCB *liveHead;
    int liveCount;

    /* statistics */
    int finishedCount;
    long totalRuntime;
    long totalWaiting;
    int lastFinishTime;
    double *wtaValues;
    int wtaCapacity;
} Scheduler;

int  scheduler_init(Scheduler *scheduler, Algorithm algorithm, int quantum,
                    int contextSwitchTime, FILE *logFile, const SchedulerHooks *hooks);

/* Simulates clock second `now` with the given arrivals (any order). */
void scheduler_tick(Scheduler *scheduler, int now, const ProcessInfo *arrivals, int arrivalCount);

/* Non-zero when no arrived process is still alive. */
int  scheduler_is_idle(const Scheduler *scheduler);

void scheduler_write_perf(const Scheduler *scheduler, FILE *out);

/* Calls visitor for each live PCB (used for emergency cleanup). */
void scheduler_for_each_live(Scheduler *scheduler, void (*visitor)(PCB *, void *), void *context);

void scheduler_destroy(Scheduler *scheduler);

const char *algorithm_name(Algorithm algorithm);

#endif
