/*
 * process_generator.c
 *
 *   1. reads processes.txt / resources.txt
 *   2. asks the user for the scheduling algorithm (and its parameters)
 *   3. creates the clock and the scheduler
 *   4. sends every process (with its resource usages) to the scheduler at the
 *      second it arrives, followed by a "second is complete" marker
 *   5. waits for the scheduler to finish and releases every IPC resource
 *
 * Usage:   ./process_generator.out                 (interactive)
 *          ./process_generator.out <algo> [quantum] [switch time]   (no prompts)
 *            algo: 1 = SJF, 2 = RR, 3 = HPF, 4 = HPF with priority demotion (bonus)
 */
#include "headers.h"
#include "common.h"
#include <ctype.h>

#define PROCESSES_FILE "processes.txt"
#define RESOURCES_FILE "resources.txt"

typedef struct
{
    int id, arrival, runtime, priority;
    int firstUse, numUses;        /* slice of the sorted resource table */
} InputProcess;

typedef struct
{
    int  pid;
    char name[MAX_RES_NAME];
    int  capture, release;
} InputUse;

static InputProcess *procs;       /* sorted by (arrival, id) */
static int numProcs;
static InputUse *uses;            /* sorted by (pid, capture) */
static int numUses;

static int   genQueue  = -1;
static int   doneQueue = -1;
static pid_t clkPid    = -1;
static pid_t schedPid  = -1;

void clearResources(int);

/* ------------------------------------------------------------------ */
/*  Input files                                                        */
/* ------------------------------------------------------------------ */
static void die(const char *msg)
{
    fprintf(stderr, "process_generator: %s\n", msg);
    exit(1);
}

static int isBlankOrComment(const char *line)
{
    while (isspace((unsigned char)*line))
        line++;
    return *line == '\0' || *line == '#';
}

static int cmpProcs(const void *a, const void *b)
{
    const InputProcess *x = a, *y = b;
    if (x->arrival != y->arrival) return x->arrival < y->arrival ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

static int cmpUses(const void *a, const void *b)
{
    const InputUse *x = a, *y = b;
    if (x->pid != y->pid)         return x->pid < y->pid ? -1 : 1;
    if (x->capture != y->capture) return x->capture < y->capture ? -1 : 1;
    return (x->release > y->release) - (x->release < y->release);
}

static void readProcesses(void)
{
    FILE *f = fopen(PROCESSES_FILE, "r");
    if (!f)
        die("cannot open " PROCESSES_FILE);

    int cap = 16;
    procs = malloc(cap * sizeof(InputProcess));
    char line[512];
    int lineNo = 0;
    while (fgets(line, sizeof line, f))
    {
        lineNo++;
        if (isBlankOrComment(line))
            continue;
        InputProcess p;
        char extra;
        if (sscanf(line, " %d %d %d %d %c", &p.id, &p.arrival, &p.runtime, &p.priority, &extra) != 4)
        {
            fprintf(stderr, "process_generator: " PROCESSES_FILE ":%d: expected <id> <arrival> <runtime> <priority>\n", lineNo);
            exit(1);
        }
        if (p.id < 1 || p.arrival < 1 || p.runtime < 1 || p.priority < 0 || p.priority > MAX_PRIORITY)
        {
            fprintf(stderr, "process_generator: " PROCESSES_FILE ":%d: need id>=1, arrival>=1, runtime>=1, 0<=priority<=%d\n",
                    lineNo, MAX_PRIORITY);
            exit(1);
        }
        if (numProcs == cap)
        {
            cap *= 2;
            procs = realloc(procs, cap * sizeof(InputProcess));
        }
        p.firstUse = p.numUses = 0;
        procs[numProcs++] = p;
    }
    fclose(f);
    qsort(procs, numProcs, sizeof(InputProcess), cmpProcs);
    for (int i = 1; i < numProcs; i++)
        if (procs[i].id == procs[i - 1].id)
            die("duplicate process id in " PROCESSES_FILE);
}

static InputProcess *findProcess(int id)
{
    for (int i = 0; i < numProcs; i++)
        if (procs[i].id == id)
            return &procs[i];
    return NULL;
}

/* resources.txt is optional (only HPF uses it). */
static void readResources(void)
{
    FILE *f = fopen(RESOURCES_FILE, "r");
    if (!f)
        return;

    int cap = 16;
    uses = malloc(cap * sizeof(InputUse));
    char line[512];
    int lineNo = 0;
    while (fgets(line, sizeof line, f))
    {
        lineNo++;
        if (isBlankOrComment(line))
            continue;
        InputUse u;
        char extra;
        if (sscanf(line, " %d %31s %d %d %c", &u.pid, u.name, &u.capture, &u.release, &extra) != 4)
        {
            fprintf(stderr, "process_generator: " RESOURCES_FILE ":%d: expected <pid> <resource> <capture> <release>\n", lineNo);
            exit(1);
        }
        InputProcess *p = findProcess(u.pid);
        if (!p)
        {
            fprintf(stderr, "process_generator: " RESOURCES_FILE ":%d: unknown process %d\n", lineNo, u.pid);
            exit(1);
        }
        if (u.capture < 1 || u.capture >= u.release || u.release > p->runtime)
        {
            fprintf(stderr, "process_generator: " RESOURCES_FILE ":%d: need 1 <= capture < release <= runtime (%d)\n",
                    lineNo, p->runtime);
            exit(1);
        }
        if (numUses == cap)
        {
            cap *= 2;
            uses = realloc(uses, cap * sizeof(InputUse));
        }
        uses[numUses++] = u;
    }
    fclose(f);

    qsort(uses, numUses, sizeof(InputUse), cmpUses);
    for (int i = 0; i < numUses; i++)
    {
        InputProcess *p = findProcess(uses[i].pid);
        if (p->numUses == 0)
            p->firstUse = i;
        p->numUses++;
        /* a process holds one resource at a time (and a release is handled before a capture) */
        if (i > 0 && uses[i - 1].pid == uses[i].pid && uses[i].capture < uses[i - 1].release)
        {
            fprintf(stderr, "process_generator: process %d uses two resources at the same time\n", uses[i].pid);
            exit(1);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  User interaction                                                   */
/* ------------------------------------------------------------------ */

/* Reads one integer in [min, max].  An empty answer (or end of input) gives defaultValue;
 * pass defaultValue < min when no default exists.  Re-asks on invalid input. */
static int askInt(const char *prompt, int defaultValue, int min, int max)
{
    char line[128];
    for (;;)
    {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
        {
            if (defaultValue >= min)
            {
                printf("\n");
                return defaultValue;
            }
            die("end of input while waiting for an answer");
        }
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
        {
            if (defaultValue >= min)
                return defaultValue;
            continue;
        }
        char *end;
        long v = strtol(p, &end, 10);
        while (isspace((unsigned char)*end))
            end++;
        if (*end == '\0' && v >= min && v <= max)
            return (int)v;
        printf("Please enter a number between %d and %d.\n", min, max);
    }
}

static void chooseAlgorithm(int argc, char *argv[], int *algo, int *quantum, int *switchTime)
{
    *quantum = 0;
    *switchTime = DEFAULT_SWITCH_TIME;

    if (argc >= 2)      /* non-interactive: <algo> [quantum] [switch time] */
    {
        *algo = atoi(argv[1]);
        if (*algo < ALGO_SJF || *algo > ALGO_HPF_DEMOTION)
            die("algorithm must be 1 (SJF), 2 (RR), 3 (HPF) or 4 (HPF with demotion)");
        if (*algo == ALGO_RR)
        {
            *quantum = argc >= 3 ? atoi(argv[2]) : 0;
            if (*quantum < 1)
                die("Round Robin needs a quantum >= 1");
            if (argc >= 4)
                *switchTime = atoi(argv[3]) < 0 ? DEFAULT_SWITCH_TIME : atoi(argv[3]);
        }
        return;
    }

    printf("Choose a scheduling algorithm:\n"
           "  1) Shortest Job First (SJF)\n"
           "  2) Round Robin (RR)\n"
           "  3) Preemptive Highest Priority First with priority inheritance (HPF)\n"
           "  4) HPF with priority demotion (bonus)\n");
    *algo = askInt("Algorithm [1-4]: ", 0, 1, 4);
    if (*algo == ALGO_RR)
    {
        *quantum    = askInt("Quantum (seconds, >= 1): ", 0, 1, 1000000);
        *switchTime = askInt("Context-switch time (seconds) [default 1]: ", DEFAULT_SWITCH_TIME, 0, 1000000);
    }
}

/* ------------------------------------------------------------------ */
/*  IPC helpers                                                        */
/* ------------------------------------------------------------------ */

/* Creates a fresh queue; a stale queue left by a crashed run is removed first. */
static int createQueue(key_t key)
{
    int old = msgget(key, 0666);
    if (old != -1)
        msgctl(old, IPC_RMID, NULL);
    int q = msgget(key, IPC_CREAT | 0666);
    if (q == -1)
    {
        perror("process_generator: cannot create a message queue");
        exit(1);
    }
    return q;
}

static int schedulerAlive(void)
{
    int status;
    return waitpid(schedPid, &status, WNOHANG) == 0;
}

/* Sends without ever blocking forever if the scheduler died.  Returns 0 on failure. */
static int sendToScheduler(GenMsg *m)
{
    for (;;)
    {
        if (msgsnd(genQueue, m, GEN_MSG_SIZE, IPC_NOWAIT) == 0)
            return 1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)            /* queue full: the scheduler is behind, wait */
        {
            if (!schedulerAlive())
                return 0;
            usleep(1000);
            continue;
        }
        perror("process_generator: msgsnd");
        return 0;
    }
}

static void killClock(void)
{
    if (clkPid > 0)
    {
        kill(clkPid, SIGINT);           /* the clock removes its shared memory */
        waitpid(clkPid, NULL, 0);
        clkPid = -1;
    }
}

static void releaseQueues(void)
{
    if (genQueue != -1)  msgctl(genQueue,  IPC_RMID, NULL);
    if (doneQueue != -1) msgctl(doneQueue, IPC_RMID, NULL);
    genQueue = doneQueue = -1;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    signal(SIGINT, clearResources);
    signal(SIGTERM, clearResources);

    /* 1. read the input files */
    readProcesses();
    readResources();
    if (numProcs == 0)
        fprintf(stderr, "process_generator: warning: " PROCESSES_FILE " has no processes\n");

    /* 2. ask for the algorithm */
    int algo, quantum, switchTime;
    chooseAlgorithm(argc, argv, &algo, &quantum, &switchTime);

    /* 3. IPC objects, then the clock and the scheduler */
    genQueue  = createQueue(GEN_QUEUE_KEY);
    doneQueue = createQueue(DONE_QUEUE_KEY);

    fflush(stdout);
    clkPid = fork();
    if (clkPid == -1)
        die("fork failed");
    if (clkPid == 0)
    {
        execl("./clk.out", "clk.out", (char *)NULL);
        perror("process_generator: cannot start ./clk.out");
        _exit(1);
    }

    char algoArg[16], quantumArg[16], switchArg[16];
    snprintf(algoArg, sizeof algoArg, "%d", algo);
    snprintf(quantumArg, sizeof quantumArg, "%d", quantum);
    snprintf(switchArg, sizeof switchArg, "%d", switchTime);
    schedPid = fork();
    if (schedPid == -1)
        die("fork failed");
    if (schedPid == 0)
    {
        execl("./scheduler.out", "scheduler.out", algoArg, quantumArg, switchArg, (char *)NULL);
        perror("process_generator: cannot start ./scheduler.out");
        _exit(1);
    }

    /* 4. connect to the clock */
    initClk();

    /* Generation main loop: at every second t, send the processes that arrive at t
     * (resource usages first, then the process), then a TICK_DONE marker so the
     * scheduler knows it has seen every arrival of second t. */
    int next = 0;
    int ok = 1;
    for (int t = 1; ok; t++)
    {
        while (getClk() < t)
        {
            if (!schedulerAlive())
            {
                ok = 0;
                break;
            }
            usleep(1000);
        }
        if (!ok)
            break;

        while (next < numProcs && procs[next].arrival == t)
        {
            InputProcess *p = &procs[next];
            for (int u = 0; u < p->numUses && ok; u++)
            {
                GenMsg m;
                memset(&m, 0, sizeof m);
                m.mtype   = t;
                m.kind    = MSG_RESOURCE;
                m.capture = uses[p->firstUse + u].capture;
                m.release = uses[p->firstUse + u].release;
                strncpy(m.resource, uses[p->firstUse + u].name, MAX_RES_NAME - 1);
                ok = sendToScheduler(&m);
            }
            GenMsg m;
            memset(&m, 0, sizeof m);
            m.mtype    = t;
            m.kind     = MSG_PROCESS;
            m.id       = p->id;
            m.arrival  = p->arrival;
            m.runtime  = p->runtime;
            m.priority = p->priority;
            ok = ok && sendToScheduler(&m);
            next++;
        }

        GenMsg tick;
        memset(&tick, 0, sizeof tick);
        tick.mtype = t;
        tick.kind  = MSG_TICK_DONE;
        tick.last  = (next == numProcs);
        ok = ok && sendToScheduler(&tick);
        if (tick.last)
            break;
    }

    /* 5. wait for the scheduler, then clear everything */
    waitpid(schedPid, NULL, 0);
    schedPid = -1;
    releaseQueues();
    destroyClk(false);
    killClock();
    free(procs);
    free(uses);
    return 0;
}

/* Called on Ctrl-C / SIGTERM: stop the scheduler (it kills its own processes)
 * and release every IPC resource. */
void clearResources(int signum)
{
    (void)signum;
    if (schedPid > 0)
    {
        kill(schedPid, SIGINT);
        waitpid(schedPid, NULL, 0);
    }
    releaseQueues();
    shmdt(shmaddr);
    killClock();
    exit(1);
}
