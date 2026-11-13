/*
 * scheduler.c - the OS scheduler
 *
 * Started by the process generator as:  ./scheduler.out <algorithm> <quantum> <switch time>
 *
 * Design
 * ------
 * The scheduler simulates the machine one clock second at a time ("logical time"
 * `now`).  For every second it waits until the clock has reached it, receives the
 * arrivals of that second from the generator, and then runs simulateTick(), which
 * applies the events in the order fixed by the project document:
 *
 *     0. a context switch that ends at this second completes (the process starts)
 *     1. the running process's own events:  release, capture, finish
 *     2. arrivals, in id order
 *     3. RR quantum expiry, preemption checks, and the choice of the next process
 *
 * Because the logical second is decoupled from the wall clock, a scheduler that is
 * a few milliseconds late still produces exactly the same log.
 *
 * Data structures
 * ---------------
 *   SJF                  binary min-heap ordered by (runtime, arrival, id)
 *   HPF / HPF+demotion   binary min-heap ordered by (effective priority, id);
 *                        a priority change re-keys one entry in O(log n)
 *                        (inheritance) or the whole heap in O(n) (aging)
 *   RR                   FIFO linked list
 *   Resource             holder + array of blocked waiters
 *
 * IPC
 * ---
 *   generator -> scheduler   message queue (arrivals + "second complete" marker)
 *   scheduler -> process     SIGSTOP / SIGCONT, plus a shared-memory counter that
 *                            holds the process's remaining time
 *   process   -> scheduler   message queue (termination notice); the scheduler
 *                            never terminates a process, it only reaps it
 *   clock     -> scheduler   shared memory (getClk)
 */
#include "headers.h"
#include "common.h"
#include "pcb.h"

/* Behaviour at the instant a context switch ends (HPF family only).
 *
 * A process that arrives while a switch is in progress (up to and including the
 * second the switch ends) simply joins the Ready set; the process being switched in
 * was chosen when the switch began.  The question is whether such an arrival may
 * preempt the process in that very second:
 *   0 (default): no.  The process that has just started always runs for at least one
 *                second; the ordinary per-second preemption check takes the newcomer
 *                into account one second later.  No "started" line is ever followed
 *                by a "stopped" line with the same timestamp.
 *   1:           yes.  The newcomer preempts at once (logged started + stopped at the
 *                same second, followed by another context switch).
 * The project document does not spell this case out; it is the one rule worth
 * confirming with the TA (see the report). Build with -DPREEMPT_AT_DISPATCH=1 to switch. */
#ifndef PREEMPT_AT_DISPATCH
#define PREEMPT_AT_DISPATCH 0
#endif

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */
static int algorithm, quantum, switchTime;
static int now;                         /* logical clock second being simulated */

static int genQueue = -1, doneQueue = -1;
static FILE *logFile;

static Heap readyHeap;                  /* SJF, HPF, HPF+demotion */
static Fifo readyFifo;                  /* RR */

static PCB *running;                    /* process on the CPU (NULL: idle or switching) */
static PCB *switchTarget;               /* process being switched in (NULL: no switch) */
static int  switchEnd;                  /* second at which the switch completes */
static int  sliceLeft;                  /* RR: seconds left in the current quantum */

static bool allArrived;                 /* the generator announced the last arrival */
static int  totalProcs, finishedCount;

static PCB **arrivals;                  /* processes that arrived in the current second */
static int numArrivals, capArrivals;

static Resource **resTable;
static int numRes, capRes;

static PCB *aliveList;                  /* every forked process, for emergency cleanup */

/* statistics */
static long   totalRuntime, totalWait;
static int    lastFinish;
static double *wtaList;
static int    wtaCap;

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */
static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL)
    {
        perror("scheduler: out of memory");
        exit(1);
    }
    return p;
}

static bool usesHeap(void)  { return algorithm != ALGO_RR; }
static bool isHpf(void)     { return algorithm == ALGO_HPF || algorithm == ALGO_HPF_DEMOTION; }

/* ------------------------------------------------------------------ */
/*  Priorities and ready queue                                         */
/* ------------------------------------------------------------------ */

/* Priority used to compare processes (smaller = higher).
 *  - Running, Blocked, or plain HPF: the priority in force (original, or inherited).
 *  - HPF with demotion, Ready process: the better of the aged priority and the
 *    inherited one.  bonus = floor((w-20)/5)+1 once the process has waited w >= 20 s. */
static int effPriority(const PCB *p)
{
    int eff = p->curPriority;
    if (algorithm == ALGO_HPF_DEMOTION && p->state == PS_READY)
    {
        int w = now - p->readySince;
        int bonus = (w < 20) ? 0 : (w - 20) / 5 + 1;
        int aged = p->priority - bonus;
        if (aged < 0)
            aged = 0;
        if (aged < eff)
            eff = aged;
    }
    return eff;
}

static int precedesSjf(const PCB *a, const PCB *b)
{
    if (a->runtime != b->runtime) return a->runtime < b->runtime;
    if (a->arrival != b->arrival) return a->arrival < b->arrival;
    return a->id < b->id;
}

static int precedesHpf(const PCB *a, const PCB *b)
{
    int ea = effPriority(a), eb = effPriority(b);
    if (ea != eb) return ea < eb;
    return a->id < b->id;
}

/* A process enters the Ready state at this second. */
static void readyPush(PCB *p)
{
    p->state = PS_READY;
    p->readySince = now;
    if (usesHeap())
        heapPush(&readyHeap, p);
    else
        fifoPush(&readyFifo, p);
}

static bool readyEmpty(void)
{
    return usesHeap() ? readyHeap.size == 0 : readyFifo.size == 0;
}

static PCB *readyPeek(void)
{
    return usesHeap() ? heapPeek(&readyHeap) : readyFifo.head;
}

static PCB *readyPop(void)
{
    return usesHeap() ? heapPop(&readyHeap) : fifoPop(&readyFifo);
}

/* ------------------------------------------------------------------ */
/*  Log                                                                */
/* ------------------------------------------------------------------ */
static void logEvent(const PCB *p, const char *state)
{
    int wait = now - p->arrival - p->executed;
    fprintf(logFile, "At time %d process %d %s arr %d total %d remain %d wait %d",
            now, p->id, state, p->arrival, p->runtime, p->remaining, wait);
}

/* ------------------------------------------------------------------ */
/*  Resources                                                          */
/* ------------------------------------------------------------------ */
static Resource *findResource(const char *name)
{
    for (int i = 0; i < numRes; i++)
        if (strcmp(resTable[i]->name, name) == 0)
            return resTable[i];
    if (numRes == capRes)
    {
        capRes = capRes ? 2 * capRes : 8;
        resTable = xrealloc(resTable, capRes * sizeof(Resource *));
    }
    Resource *r = calloc(1, sizeof(Resource));
    if (r == NULL)
    {
        perror("scheduler: out of memory");
        exit(1);
    }
    strncpy(r->name, name, MAX_RES_NAME - 1);
    resTable[numRes++] = r;
    return r;
}

static void addWaiter(Resource *r, PCB *p)
{
    if (r->numWaiters == r->capWaiters)
    {
        r->capWaiters = r->capWaiters ? 2 * r->capWaiters : 4;
        r->waiters = xrealloc(r->waiters, r->capWaiters * sizeof(PCB *));
    }
    r->waiters[r->numWaiters++] = p;
}

/* Index of the waiter that gets the resource: best original priority, then smaller id. */
static int bestWaiter(const Resource *r)
{
    int best = 0;
    for (int i = 1; i < r->numWaiters; i++)
    {
        const PCB *a = r->waiters[i], *b = r->waiters[best];
        if (a->priority < b->priority || (a->priority == b->priority && a->id < b->id))
            best = i;
    }
    return best;
}

/* The running process p releases the resource it holds.  The resource goes directly
 * to the best blocked waiter, which becomes Ready (it must still be scheduled), and
 * p loses any priority it inherited through the resource. */
static void releaseResource(PCB *p)
{
    Resource *r = p->uses[p->heldUse].res;
    p->heldUse = -1;
    p->curPriority = p->priority;
    r->holder = NULL;

    if (r->numWaiters == 0)
        return;

    int idx = bestWaiter(r);
    PCB *w = r->waiters[idx];
    r->waiters[idx] = r->waiters[--r->numWaiters];

    r->holder = w;
    w->waitingOn = NULL;
    w->heldUse = w->nextUse++;          /* the pending capture is now done */

    /* the new holder inherits from the processes still waiting (never better than
     * its own priority here, but computed generally) */
    w->curPriority = w->priority;
    for (int i = 0; i < r->numWaiters; i++)
        if (r->waiters[i]->priority < w->curPriority)
            w->curPriority = r->waiters[i]->priority;

    readyPush(w);
}

/* ------------------------------------------------------------------ */
/*  Process control                                                    */
/* ------------------------------------------------------------------ */
static void aliveInsert(PCB *p)
{
    p->prevAlive = NULL;
    p->nextAlive = aliveList;
    if (aliveList)
        aliveList->prevAlive = p;
    aliveList = p;
}

static void aliveRemove(PCB *p)
{
    if (p->prevAlive) p->prevAlive->nextAlive = p->nextAlive; else aliveList = p->nextAlive;
    if (p->nextAlive) p->nextAlive->prevAlive = p->prevAlive;
}

/* Fork the process and give it its parameters. */
static void startProcess(PCB *p)
{
    p->shmid = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | 0600);
    if (p->shmid == -1)
    {
        perror("scheduler: shmget");
        exit(1);
    }
    p->remainingShm = (int *)shmat(p->shmid, NULL, 0);
    if (p->remainingShm == (int *)-1)
    {
        perror("scheduler: shmat");
        exit(1);
    }
    *p->remainingShm = p->remaining;

    char idArg[16], shmArg[16];
    snprintf(idArg, sizeof idArg, "%d", p->id);
    snprintf(shmArg, sizeof shmArg, "%d", p->shmid);
    fflush(logFile);
    p->pid = fork();
    if (p->pid == -1)
    {
        perror("scheduler: fork");
        exit(1);
    }
    if (p->pid == 0)
    {
        execl("./process.out", "process.out", idArg, shmArg, (char *)NULL);
        perror("scheduler: cannot start ./process.out");
        _exit(1);
    }
    p->started = true;
    aliveInsert(p);
}

/* Put p on the CPU: fork it the first time, otherwise continue it. */
static void dispatch(PCB *p)
{
    if (!p->started)
    {
        startProcess(p);
        logEvent(p, "started");
    }
    else
    {
        kill(p->pid, SIGCONT);
        logEvent(p, "resumed");
    }
    fputc('\n', logFile);
    p->state = PS_RUNNING;
    running = p;
    sliceLeft = quantum;
}

/* Take the running process off the CPU because it was preempted. */
static void preemptRunning(void)
{
    PCB *p = running;
    kill(p->pid, SIGSTOP);
    logEvent(p, "stopped");
    fputc('\n', logFile);
    running = NULL;
    readyPush(p);
}

/* The running process asked for a resource somebody else holds: it blocks, and the
 * holder inherits its priority if that is higher. */
static void blockRunning(PCB *p, Resource *r)
{
    kill(p->pid, SIGSTOP);
    logEvent(p, "stopped");
    fputc('\n', logFile);

    p->state = PS_BLOCKED;
    p->waitingOn = r;
    addWaiter(r, p);
    running = NULL;

    PCB *holder = r->holder;
    if (p->curPriority < holder->curPriority)
    {
        holder->curPriority = p->curPriority;
        if (usesHeap())
            heapFix(&readyHeap, holder);       /* no-op unless the holder is queued */
    }
}

/* The running process consumed all its runtime: wait for its termination notice,
 * reap it, log it, update the statistics and delete its PCB. */
static void finishProcess(PCB *p)
{
    DoneMsg done;
    while (msgrcv(doneQueue, &done, DONE_MSG_SIZE, p->id, 0) == -1)
    {
        if (errno == EINTR)
            continue;
        perror("scheduler: waiting for a termination notice");
        break;
    }
    waitpid(p->pid, NULL, 0);
    shmdt(p->remainingShm);
    shmctl(p->shmid, IPC_RMID, NULL);

    int ta = now - p->arrival;
    double wta = (double)ta / p->runtime;
    int wait = ta - p->runtime;

    logEvent(p, "finished");
    fprintf(logFile, " TA %d WTA %.2f\n", ta, wta);

    totalRuntime += p->runtime;
    totalWait += wait;
    lastFinish = now;
    if (finishedCount == wtaCap)
    {
        wtaCap = wtaCap ? 2 * wtaCap : 64;
        wtaList = xrealloc(wtaList, wtaCap * sizeof(double));
    }
    wtaList[finishedCount++] = wta;

    aliveRemove(p);
    running = NULL;
    free(p->uses);
    free(p);
}

/* ------------------------------------------------------------------ */
/*  Arrivals                                                           */
/* ------------------------------------------------------------------ */

/* Receive every message of the current second, up to and including the marker. */
static void receiveArrivals(void)
{
    ResourceUse *pending = NULL;
    int numPending = 0, capPending = 0;

    numArrivals = 0;
    for (;;)
    {
        GenMsg m;
        if (msgrcv(genQueue, &m, GEN_MSG_SIZE, now, 0) == -1)
        {
            if (errno == EINTR)
                continue;
            perror("scheduler: receiving arrivals");
            exit(1);
        }

        if (m.kind == MSG_RESOURCE)
        {
            if (numPending == capPending)
            {
                capPending = capPending ? 2 * capPending : 4;
                pending = xrealloc(pending, capPending * sizeof(ResourceUse));
            }
            memcpy(pending[numPending].name, m.resource, MAX_RES_NAME);
            pending[numPending].name[MAX_RES_NAME - 1] = '\0';
            pending[numPending].capture = m.capture;
            pending[numPending].release = m.release;
            numPending++;
        }
        else if (m.kind == MSG_PROCESS)
        {
            PCB *p = calloc(1, sizeof(PCB));
            if (p == NULL)
            {
                perror("scheduler: out of memory");
                exit(1);
            }
            p->id = m.id;
            p->arrival = m.arrival;
            p->runtime = m.runtime;
            p->priority = p->curPriority = m.priority;
            p->remaining = m.runtime;
            p->heapPos = -1;
            p->heldUse = -1;
            p->pid = -1;
            if (numPending > 0)
            {
                p->uses = malloc(numPending * sizeof(Usage));
                if (p->uses == NULL)
                {
                    perror("scheduler: out of memory");
                    exit(1);
                }
                for (int i = 0; i < numPending; i++)
                {
                    p->uses[i].res = findResource(pending[i].name);
                    p->uses[i].capture = pending[i].capture;
                    p->uses[i].release = pending[i].release;
                }
                p->numUses = numPending;
                numPending = 0;
            }
            if (numArrivals == capArrivals)
            {
                capArrivals = capArrivals ? 2 * capArrivals : 8;
                arrivals = xrealloc(arrivals, capArrivals * sizeof(PCB *));
            }
            arrivals[numArrivals++] = p;
            totalProcs++;
        }
        else    /* MSG_TICK_DONE: everything of this second has been received */
        {
            if (m.last)
                allArrived = true;
            break;
        }
    }
    free(pending);
}

/* ------------------------------------------------------------------ */
/*  One simulated second                                               */
/* ------------------------------------------------------------------ */

/* The running process executes for one more second and its own events happen.
 * Returns true when it left the CPU (blocked or finished). */
static bool runOneSecond(void)
{
    PCB *p = running;
    p->executed++;
    p->remaining--;
    *p->remainingShm = p->remaining;
    sliceLeft--;

    /* a release comes first, then a capture, then the finish */
    if (p->heldUse >= 0 && p->uses[p->heldUse].release == p->executed)
        releaseResource(p);

    if (p->nextUse < p->numUses && p->uses[p->nextUse].capture == p->executed)
    {
        Resource *r = p->uses[p->nextUse].res;
        if (r->holder == NULL)
        {
            r->holder = p;
            p->heldUse = p->nextUse++;
        }
        else
        {
            blockRunning(p, r);
            return true;
        }
    }

    if (p->remaining == 0)
    {
        finishProcess(p);
        return true;
    }
    return false;
}

static void simulateTick(void)
{
    bool cpuVacated = false;       /* the CPU left a process at this very second */
    bool justDispatched = false;   /* a context switch completed at this second  */

    /* HPF with demotion: the effective priorities depend on the second */
    if (algorithm == ALGO_HPF_DEMOTION)
        heapRebuild(&readyHeap);

    /* 0. end of a context switch: the chosen process starts or resumes */
    if (switchTarget != NULL && now >= switchEnd)
    {
        dispatch(switchTarget);
        switchTarget = NULL;
        justDispatched = true;
    }

    /* 1. the running process executed during the last second */
    if (running != NULL && !justDispatched)
        cpuVacated = runOneSecond();

    /* 2. arrivals, in id order */
    for (int i = 0; i < numArrivals; i++)
        readyPush(arrivals[i]);
    numArrivals = 0;

    /* 3a. Round Robin: the quantum expired */
    if (algorithm == ALGO_RR && running != NULL && sliceLeft == 0)
    {
        if (!readyEmpty())
        {
            preemptRunning();
            cpuVacated = true;
        }
        else
            sliceLeft = quantum;       /* nobody is waiting: keep going, no log, no switch */
    }

    /* 3b. HPF: a strictly higher-priority Ready process preempts the running one */
    if (isHpf() && running != NULL && !readyEmpty() && (PREEMPT_AT_DISPATCH || !justDispatched))
    {
        if (effPriority(readyPeek()) < effPriority(running))
        {
            preemptRunning();
            cpuVacated = true;
        }
    }

    /* 3c. choose the next process.  Moving the CPU directly from one process to
     * another costs a context switch; an idle CPU starts the process at once. */
    if (running == NULL && switchTarget == NULL && !readyEmpty())
    {
        PCB *next = readyPop();
        if (cpuVacated && switchTime > 0)
        {
            switchTarget = next;
            switchEnd = now + switchTime;
        }
        else
            dispatch(next);
    }
}

/* ------------------------------------------------------------------ */
/*  Output                                                             */
/* ------------------------------------------------------------------ */
static void writePerf(void)
{
    int utilization = lastFinish > 0 ? (int)(totalRuntime * 100 / lastFinish) : 0;   /* floor */
    int n = finishedCount;
    double avgWta = 0, stdWta = 0;
    int avgWait = 0;

    if (n > 0)
    {
        double sum = 0;
        for (int i = 0; i < n; i++)
            sum += wtaList[i];
        avgWta = sum / n;
        double var = 0;
        for (int i = 0; i < n; i++)
            var += (wtaList[i] - avgWta) * (wtaList[i] - avgWta);
        stdWta = sqrt(var / n);                 /* population standard deviation */
        avgWait = (int)(totalWait / n);         /* floor */
    }

    FILE *perf = fopen("scheduler.perf", "w");
    if (!perf)
    {
        perror("scheduler: cannot write scheduler.perf");
        return;
    }
    fprintf(perf, "CPU utilization = %d%%\n", utilization);
    fprintf(perf, "Avg WTA = %.2f\n", avgWta);
    fprintf(perf, "Avg Waiting = %d\n", avgWait);
    fprintf(perf, "Std WTA = %.2f\n", stdWta);
    fclose(perf);
}

/* Ctrl-C / SIGTERM: kill the simulated processes and free their memory. */
static void emergencyCleanup(int signum)
{
    (void)signum;
    for (PCB *p = aliveList; p; p = p->nextAlive)
    {
        if (p->pid > 0)
        {
            kill(p->pid, SIGKILL);           /* SIGKILL also works on stopped processes */
            waitpid(p->pid, NULL, 0);        /* reap it so it does not stay a zombie */
        }
        if (p->shmid > 0)
            shmctl(p->shmid, IPC_RMID, NULL);
    }
    if (logFile)
        fflush(logFile);
    _exit(1);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <algorithm 1-4> <quantum> <context-switch time>\n", argv[0]);
        return 1;
    }
    algorithm  = atoi(argv[1]);
    quantum    = atoi(argv[2]);
    switchTime = atoi(argv[3]);
    if (algorithm < ALGO_SJF || algorithm > ALGO_HPF_DEMOTION)
    {
        fprintf(stderr, "scheduler: unknown algorithm %d\n", algorithm);
        return 1;
    }
    if (algorithm == ALGO_RR)
    {
        if (quantum < 1)
        {
            fprintf(stderr, "scheduler: Round Robin needs a quantum >= 1\n");
            return 1;
        }
        if (switchTime < 0)
            switchTime = DEFAULT_SWITCH_TIME;
    }
    else
        switchTime = DEFAULT_SWITCH_TIME;    /* fixed at 1 second for SJF and HPF */

    signal(SIGINT, emergencyCleanup);
    signal(SIGTERM, emergencyCleanup);

    initClk();

    genQueue  = msgget(GEN_QUEUE_KEY, 0666);
    doneQueue = msgget(DONE_QUEUE_KEY, 0666);
    if (genQueue == -1 || doneQueue == -1)
    {
        perror("scheduler: cannot open the message queues");
        return 1;
    }

    logFile = fopen("scheduler.log", "w");
    if (!logFile)
    {
        perror("scheduler: cannot write scheduler.log");
        return 1;
    }
    fprintf(logFile, "#At time x process y state arr w total z remain y wait k\n");

    heapInit(&readyHeap, algorithm == ALGO_SJF ? precedesSjf : precedesHpf);

    /* Main loop: one iteration per clock second until every process has finished. */
    for (now = 1; ; now++)
    {
        while (getClk() < now)
            usleep(1000);
        if (!allArrived)
            receiveArrivals();
        simulateTick();
        if (allArrived && finishedCount == totalProcs)
            break;
    }

    fclose(logFile);
    logFile = NULL;
    writePerf();

    /* free memory */
    free(readyHeap.items);
    free(arrivals);
    free(wtaList);
    for (int i = 0; i < numRes; i++)
    {
        free(resTable[i]->waiters);
        free(resTable[i]);
    }
    free(resTable);

    destroyClk(false);   /* the generator terminates the clock and removes the queues */
    return 0;
}
