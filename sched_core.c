/*
 * sched_core.c
 * Scheduling engine: SJF, RR, preemptive HPF with priority inheritance and
 * HPF with priority aging/demotion (bonus).
 *
 * Every clock second is processed in the order required by the document:
 *   1. the running process's own events: release, capture, finish
 *   2. arrivals, in ID order
 *   3. context-switch completion, RR quantum expiry, preemption checks and
 *      the choice of the next process
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sched_core.h"

#define AGING_START 20 /* seconds of Ready waiting before the first bonus */
#define AGING_STEP 5   /* one more bonus level every AGING_STEP seconds   */

/*
 * Compile-time switches for the TA edge case where a higher-priority process
 * arrives or ages in the exact second a context switch ends:
 *   - Default (both 0): target is chosen when the switch begins, dispatched
 *     when the switch ends, and runs at least 1 second before preemption.
 *   - -DPREEMPT_AT_DISPATCH=1: target is dispatched at switchEnd and may be
 *     immediately preempted at the same second (logged started + stopped).
 *   - -DCHOOSE_AT_SWITCH_END=1: target is popped from Ready at switchEnd
 *     (after arrivals and aging at switchEnd).
 */
#ifndef PREEMPT_AT_DISPATCH
#define PREEMPT_AT_DISPATCH 0
#endif
#ifndef CHOOSE_AT_SWITCH_END
#define CHOOSE_AT_SWITCH_END 0
#endif

/* ------------------------------------------------------------------ */
/*                           Small helpers                             */
/* ------------------------------------------------------------------ */

static int min_int(int a, int b) { return a < b ? a : b; }

static int is_hpf(const Scheduler *s)
{
    return s->algorithm == ALGO_HPF || s->algorithm == ALGO_HPF_AGING;
}

static void call_hook(void (*hook)(PCB *, void *), PCB *pcb, void *context)
{
    if (hook)
        hook(pcb, context);
}

const char *algorithm_name(Algorithm algorithm)
{
    switch (algorithm) {
    case ALGO_SJF: return "Shortest Job First";
    case ALGO_RR: return "Round Robin";
    case ALGO_HPF: return "Preemptive HPF with Priority Inheritance";
    case ALGO_HPF_AGING: return "HPF with Priority Demotion (aging)";
    }
    return "Unknown";
}

/* SJF order: runtime, then arrival, then ID. */
static int sjf_less(const PCB *a, const PCB *b)
{
    if (a->runtime != b->runtime)
        return a->runtime < b->runtime;
    if (a->arrivalTime != b->arrivalTime)
        return a->arrivalTime < b->arrivalTime;
    return a->id < b->id;
}

/* HPF order: cached comparison priority, then ID. */
static int hpf_less(const PCB *a, const PCB *b)
{
    if (a->priorityKey != b->priorityKey)
        return a->priorityKey < b->priorityKey;
    return a->id < b->id;
}

/* Priority of the running process: original or inherited (no aging). */
static int running_priority(const PCB *pcb)
{
    return min_int(pcb->originalPriority, pcb->inheritedPriority);
}

/* Priority of a Ready process: aged priority (bonus only) or inherited. */
static int ready_priority(const Scheduler *s, const PCB *pcb, int now)
{
    int priority = pcb->originalPriority;
    if (s->algorithm == ALGO_HPF_AGING) {
        int waited = now - pcb->readySince; /* w: consecutive Ready seconds */
        if (waited >= AGING_START) {
            int bonus = (waited - AGING_START) / AGING_STEP + 1;
            priority = priority - bonus < 0 ? 0 : priority - bonus;
        }
    }
    return min_int(priority, pcb->inheritedPriority);
}

/* ------------------------------------------------------------------ */
/*                              Logging                                */
/* ------------------------------------------------------------------ */

static void log_event(Scheduler *s, const PCB *pcb, const char *event, int now)
{
    if (!s->logFile)
        return;
    int remaining = pcb->runtime - pcb->executedTime;
    int wait = now - pcb->arrivalTime - pcb->executedTime;
    fprintf(s->logFile, "At time %d process %d %s arr %d total %d remain %d wait %d",
            now, pcb->id, event, pcb->arrivalTime, pcb->runtime, remaining, wait);
    if (pcb->state == STATE_FINISHED) {
        int turnaround = now - pcb->arrivalTime;
        fprintf(s->logFile, " TA %d WTA %.2f", turnaround, (double)turnaround / pcb->runtime);
    }
    fputc('\n', s->logFile);
    fflush(s->logFile);
}

/* ------------------------------------------------------------------ */
/*                         Ready-queue access                          */
/* ------------------------------------------------------------------ */

static void make_ready(Scheduler *s, PCB *pcb, int now)
{
    pcb->state = STATE_READY;
    pcb->readySince = now; /* aging counter w restarts */
    if (s->algorithm == ALGO_RR) {
        fifo_push(&s->readyQueue, pcb);
    } else {
        if (is_hpf(s))
            pcb->priorityKey = ready_priority(s, pcb, now);
        heap_push(&s->readyHeap, pcb);
    }
}

static int ready_is_empty(const Scheduler *s)
{
    return s->algorithm == ALGO_RR ? s->readyQueue.size == 0 : s->readyHeap.size == 0;
}

static PCB *ready_peek(const Scheduler *s)
{
    return s->algorithm == ALGO_RR ? fifo_peek(&s->readyQueue) : heap_peek(&s->readyHeap);
}

static PCB *ready_pop(Scheduler *s)
{
    return s->algorithm == ALGO_RR ? fifo_pop(&s->readyQueue) : heap_pop(&s->readyHeap);
}

/* Bonus: aged priorities change with time, so refresh all keys each tick. */
static void refresh_aged_priorities(Scheduler *s, int now)
{
    int changed = 0;
    for (int i = 0; i < s->readyHeap.size; i++) {
        PCB *pcb = s->readyHeap.items[i];
        int key = ready_priority(s, pcb, now);
        if (key != pcb->priorityKey) {
            pcb->priorityKey = key;
            changed = 1;
        }
    }
    if (changed)
        heap_rebuild(&s->readyHeap);
}

/* Recomputes inheritance of a resource holder after its waiters changed. */
static void update_inheritance(Scheduler *s, PCB *holder, int now)
{
    if (!holder)
        return;
    holder->inheritedPriority = holder->heldResource
                                    ? resource_best_waiter_priority(holder->heldResource)
                                    : NO_INHERITED_PRIORITY;
    if (holder->state == STATE_READY && holder->heapIndex >= 0) {
        holder->priorityKey = ready_priority(s, holder, now);
        heap_update(&s->readyHeap, holder);
    }
}

/* ------------------------------------------------------------------ */
/*                       PCB creation / deletion                       */
/* ------------------------------------------------------------------ */

static PCB *create_pcb(Scheduler *s, const ProcessInfo *info)
{
    PCB *pcb = calloc(1, sizeof(PCB));
    if (!pcb)
        return NULL;
    pcb->id = info->id;
    pcb->arrivalTime = info->arrival;
    pcb->runtime = info->runtime;
    pcb->originalPriority = info->priority;
    pcb->inheritedPriority = NO_INHERITED_PRIORITY;
    pcb->heapIndex = -1;
    pcb->systemPid = -1;
    pcb->sharedMemoryId = -1;

    if (s->usesResources && info->resourceCount > 0 && info->resources != NULL) {
        ProcessInfo normalized;
        memset(&normalized, 0, sizeof normalized);
        normalized.id = info->id;
        normalized.runtime = info->runtime;
        for (int i = 0; i < info->resourceCount; i++) {
            if (info->resources[i].name)
                process_add_resource(&normalized, info->resources[i].name,
                                     info->resources[i].captureTime,
                                     info->resources[i].releaseTime);
        }
        normalize_resource_requests(&normalized);
        if (normalized.resourceCount > 0) {
            pcb->resourceUses = calloc((size_t)normalized.resourceCount, sizeof(ResourceUse));
            if (pcb->resourceUses) {
                for (int i = 0; i < normalized.resourceCount; i++) {
                    pcb->resourceUses[i].resource =
                        resource_table_get(&s->resources, normalized.resources[i].name);
                    pcb->resourceUses[i].captureTime = normalized.resources[i].captureTime;
                    pcb->resourceUses[i].releaseTime = normalized.resources[i].releaseTime;
                }
                pcb->resourceUseCount = normalized.resourceCount;
            }
        }
        free_process_resources(&normalized);
    }

    /* insert into the live list */
    pcb->nextLive = s->liveHead;
    if (s->liveHead)
        s->liveHead->prevLive = pcb;
    s->liveHead = pcb;
    s->liveCount++;
    return pcb;
}

static void delete_pcb(Scheduler *s, PCB *pcb)
{
    if (pcb->prevLive)
        pcb->prevLive->nextLive = pcb->nextLive;
    else
        s->liveHead = pcb->nextLive;
    if (pcb->nextLive)
        pcb->nextLive->prevLive = pcb->prevLive;
    s->liveCount--;
    free(pcb->resourceUses);
    free(pcb);
}

/* ------------------------------------------------------------------ */
/*                         CPU state transitions                       */
/* ------------------------------------------------------------------ */

/* Puts a process on the CPU (start or resume). Demotes it (bonus). */
static void dispatch(Scheduler *s, PCB *pcb, int now)
{
    pcb->state = STATE_RUNNING;
    pcb->readySince = now; /* aging counter reset: back to original priority */
    s->running = pcb;
    if (!pcb->hasStarted) {
        pcb->hasStarted = 1;
        log_event(s, pcb, "started", now);
        call_hook(s->hooks.onStart, pcb, s->hooks.context);
    } else {
        log_event(s, pcb, "resumed", now);
        call_hook(s->hooks.onResume, pcb, s->hooks.context);
    }
    if (s->algorithm == ALGO_RR)
        s->quantumEnd = now + s->quantum;
}

/* Takes the running process off the CPU (it stays alive). */
static void stop_running(Scheduler *s, int now)
{
    PCB *pcb = s->running;
    log_event(s, pcb, "stopped", now);
    call_hook(s->hooks.onStop, pcb, s->hooks.context);
    s->running = NULL;
    s->cpuReleasedAt = now;
}

/* Running process is preempted (quantum expiry or higher priority). */
static void preempt_running(Scheduler *s, int now)
{
    PCB *pcb = s->running;
    stop_running(s, now);
    make_ready(s, pcb, now);
}

/* Running process requests a resource held by someone else. */
static void block_running(Scheduler *s, Resource *resource, int releaseTime, int now)
{
    PCB *pcb = s->running;
    stop_running(s, now);
    pcb->state = STATE_BLOCKED;
    pcb->waitingFor = resource;
    pcb->pendingReleaseTime = releaseTime;
    resource_add_waiter(resource, pcb);
    update_inheritance(s, resource->holder, now); /* holder inherits */
}

/* Running process releases its resource; it goes to the best waiter. */
static void release_resource(Scheduler *s, PCB *pcb, int now)
{
    Resource *resource = pcb->heldResource;
    pcb->heldResource = NULL;
    pcb->inheritedPriority = NO_INHERITED_PRIORITY; /* back to original */
    resource->holder = NULL;

    PCB *next = resource_pop_waiter(resource);
    if (next) {
        resource->holder = next;
        next->heldResource = resource;
        next->heldReleaseTime = next->pendingReleaseTime;
        next->waitingFor = NULL;
        next->inheritedPriority = resource_best_waiter_priority(resource);
        make_ready(s, next, now);
    }
}

static void record_finish_statistics(Scheduler *s, const PCB *pcb, int now)
{
    int turnaround = now - pcb->arrivalTime;
    if (s->finishedCount == s->wtaCapacity) {
        int newCapacity = s->wtaCapacity ? s->wtaCapacity * 2 : 16;
        double *grown = realloc(s->wtaValues, newCapacity * sizeof(double));
        if (!grown)
            return;
        s->wtaValues = grown;
        s->wtaCapacity = newCapacity;
    }
    s->wtaValues[s->finishedCount++] = (double)turnaround / pcb->runtime;
    s->totalRuntime += pcb->runtime;
    s->totalWaiting += now - pcb->arrivalTime - pcb->runtime;
    if (now > s->lastFinishTime)
        s->lastFinishTime = now;
}

static void finish_running(Scheduler *s, int now)
{
    PCB *pcb = s->running;
    if (pcb->heldResource) /* cannot happen with valid input */
        release_resource(s, pcb, now);
    pcb->state = STATE_FINISHED;
    call_hook(s->hooks.onFinish, pcb, s->hooks.context); /* wait for process termination notice first */
    log_event(s, pcb, "finished", now);
    record_finish_statistics(s, pcb, now);
    s->running = NULL;
    s->cpuReleasedAt = now;
    delete_pcb(s, pcb);
}

/* Step 1: the running process executed one more second. */
static void advance_running(Scheduler *s, int now)
{
    PCB *pcb = s->running;
    pcb->executedTime++;
    call_hook(s->hooks.onExecute, pcb, s->hooks.context);

    /* release first ... */
    if (pcb->heldResource && pcb->executedTime == pcb->heldReleaseTime)
        release_resource(s, pcb, now);

    /* ... then capture ... */
    if (pcb->nextResourceUse < pcb->resourceUseCount &&
        pcb->executedTime == pcb->resourceUses[pcb->nextResourceUse].captureTime) {
        ResourceUse *use = &pcb->resourceUses[pcb->nextResourceUse++];
        Resource *resource = use->resource;
        if (resource->holder == NULL) {
            resource->holder = pcb;
            pcb->heldResource = resource;
            pcb->heldReleaseTime = use->releaseTime;
        } else {
            block_running(s, resource, use->releaseTime, now);
            return; /* capture < runtime, so it cannot also finish now */
        }
    }

    /* ... then finish */
    if (pcb->executedTime == pcb->runtime)
        finish_running(s, now);
}

/* Picks the next process when the CPU is free. A process leaving the CPU at
 * this very second means a context switch; an idle CPU dispatches at once. */
static void choose_next(Scheduler *s, int now)
{
    if (s->running || s->switching || ready_is_empty(s))
        return;
    if (s->cpuReleasedAt == now && s->contextSwitchTime > 0) {
        s->switching = 1;
        s->switchTarget = CHOOSE_AT_SWITCH_END ? NULL : ready_pop(s);
        s->switchEnd = now + s->contextSwitchTime;
    } else {
        PCB *next = ready_pop(s);
        dispatch(s, next, now);
    }
}

/* ------------------------------------------------------------------ */
/*                             Public API                              */
/* ------------------------------------------------------------------ */

int scheduler_init(Scheduler *s, Algorithm algorithm, int quantum,
                   int contextSwitchTime, FILE *logFile, const SchedulerHooks *hooks)
{
    memset(s, 0, sizeof *s);
    s->algorithm = algorithm;
    s->quantum = quantum > 0 ? quantum : 1;
    s->contextSwitchTime = contextSwitchTime >= 0 ? contextSwitchTime : 1;
    s->usesResources = is_hpf(s);
    s->logFile = logFile;
    s->cpuReleasedAt = -1;
    if (hooks)
        s->hooks = *hooks;
    fifo_init(&s->readyQueue);
    resource_table_init(&s->resources);
    if (!heap_init(&s->readyHeap, algorithm == ALGO_SJF ? sjf_less : hpf_less))
        return 0;
    if (logFile) {
        fprintf(logFile, "#At time x process y state arr w total z remain y wait k\n");
        fflush(logFile);
    }
    return 1;
}

/* Small insertion sort: arrivals of one second are handled in ID order. */
static void sort_arrivals_by_id(const ProcessInfo **order, int count)
{
    for (int i = 1; i < count; i++) {
        const ProcessInfo *key = order[i];
        int j = i - 1;
        while (j >= 0 && order[j]->id > key->id) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

void scheduler_tick(Scheduler *s, int now, const ProcessInfo *arrivals, int arrivalCount)
{
    s->currentTime = now;

    /* 1. running process's own events (release, capture, finish) */
    if (s->running)
        advance_running(s, now);

    /* 2. arrivals, in ID order */
    if (arrivalCount > 0) {
        const ProcessInfo **order = malloc(arrivalCount * sizeof(ProcessInfo *));
        if (order) {
            for (int i = 0; i < arrivalCount; i++)
                order[i] = &arrivals[i];
            sort_arrivals_by_id(order, arrivalCount);
            for (int i = 0; i < arrivalCount; i++) {
                PCB *pcb = create_pcb(s, order[i]);
                if (pcb)
                    make_ready(s, pcb, now);
            }
            free(order);
        }
    }

    /* 3a. a context switch that ends now dispatches its chosen target */
    int justDispatched = 0;
    if (s->switching && now >= s->switchEnd) {
        s->switching = 0;
        if (s->switchTarget) {
            dispatch(s, s->switchTarget, now);
            s->switchTarget = NULL;
            justDispatched = 1;
        }
    }

    if (s->algorithm == ALGO_HPF_AGING)
        refresh_aged_priorities(s, now);

    /* 3b. RR quantum expiry / HPF preemption check */
    if (s->running) {
        if (s->algorithm == ALGO_RR && now >= s->quantumEnd) {
            if (ready_is_empty(s))
                s->quantumEnd = now + s->quantum; /* keeps running, no log */
            else
                preempt_running(s, now);
        } else if (is_hpf(s) && (PREEMPT_AT_DISPATCH || !justDispatched) &&
                   !ready_is_empty(s) &&
                   ready_peek(s)->priorityKey < running_priority(s->running)) {
            preempt_running(s, now);
        }
    }

    /* 3c. choose the next process if the CPU is free */
    choose_next(s, now);
}

int scheduler_is_idle(const Scheduler *s)
{
    return s->liveCount == 0;
}

void scheduler_write_perf(const Scheduler *s, FILE *out)
{
    int count = s->finishedCount;
    long utilization = 0;
    long averageWaiting = 0;
    double averageWta = 0.0;
    double stdWta = 0.0;

    if (count > 0) {
        if (s->lastFinishTime > 0)
            utilization = (s->totalRuntime * 100) / s->lastFinishTime; /* floor */
        averageWaiting = s->totalWaiting / count;                      /* floor */

        double sum = 0.0;
        for (int i = 0; i < count; i++)
            sum += s->wtaValues[i];
        averageWta = sum / count;

        double squaredDeviations = 0.0;
        for (int i = 0; i < count; i++)
            squaredDeviations += (s->wtaValues[i] - averageWta) * (s->wtaValues[i] - averageWta);
        stdWta = sqrt(squaredDeviations / count); /* population std */
    }

    fprintf(out, "CPU utilization = %ld%%\n", utilization);
    fprintf(out, "Avg WTA = %.2f\n", averageWta);
    fprintf(out, "Avg Waiting = %ld\n", averageWaiting);
    fprintf(out, "Std WTA = %.2f\n", stdWta);
}

void scheduler_for_each_live(Scheduler *s, void (*visitor)(PCB *, void *), void *context)
{
    for (PCB *pcb = s->liveHead; pcb; pcb = pcb->nextLive)
        visitor(pcb, context);
}

void scheduler_destroy(Scheduler *s)
{
    while (s->liveHead)
        delete_pcb(s, s->liveHead);
    s->running = NULL;
    s->switchTarget = NULL;
    s->switching = 0;
    heap_free(&s->readyHeap);
    resource_table_free(&s->resources);
    free(s->wtaValues);
    s->wtaValues = NULL;
}
