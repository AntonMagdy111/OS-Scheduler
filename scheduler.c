/*
 * scheduler.c
 * The scheduler process: connects the scheduling engine (sched_core.c) to
 * the real system.
 *
 *  - Receives arriving processes from the process generator (message queue).
 *    For every clock second the generator sends the arrivals of that second
 *    followed by a TICK_END message, so the scheduler never misses or
 *    reorders an arrival.
 *  - Forks process.out on first dispatch, SIGSTOP/SIGCONT to switch.
 *  - Keeps each process's remaining time in a private shared memory segment.
 *  - Waits for the process's own termination message (it never kills it).
 *  - Writes scheduler.log and scheduler.perf.
 *
 * Usage (normally launched by process_generator.out):
 *   scheduler.out <algorithm 1-4> [quantum] [context_switch]
 */
#include "headers.h"
#include "sched_core.h"

static Scheduler scheduler;
static int schedulerInitialized = 0;
static int generatorQueueId = -1;
static int processQueueId = -1;
static FILE *logFile = NULL;

/* ------------------------------------------------------------------ */
/*                    OS hooks called by the engine                    */
/* ------------------------------------------------------------------ */

static void release_process_memory(PCB *pcb)
{
    if (pcb->sharedRemainingTime) {
        shmdt((const void *)pcb->sharedRemainingTime);
        pcb->sharedRemainingTime = NULL;
    }
    if (pcb->sharedMemoryId != -1) {
        shmctl(pcb->sharedMemoryId, IPC_RMID, NULL);
        pcb->sharedMemoryId = -1;
    }
}

/* Start a new process: fork it and give it its parameters. */
static void on_start(PCB *pcb, void *context)
{
    (void)context;
    pcb->sharedMemoryId = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | 0666);
    if (pcb->sharedMemoryId == -1) {
        perror("scheduler: shmget");
        return;
    }
    int *remaining = (int *)shmat(pcb->sharedMemoryId, NULL, 0);
    if ((long)remaining == -1) {
        perror("scheduler: shmat");
        release_process_memory(pcb);
        return;
    }
    *remaining = pcb->runtime - pcb->executedTime;
    pcb->sharedRemainingTime = remaining;

    pid_t pid = fork();
    if (pid == -1) {
        perror("scheduler: fork");
        return;
    }
    if (pid == 0) {
        char idText[16], runtimeText[16], shmText[16];
        snprintf(idText, sizeof idText, "%d", pcb->id);
        snprintf(runtimeText, sizeof runtimeText, "%d", pcb->runtime);
        snprintf(shmText, sizeof shmText, "%d", pcb->sharedMemoryId);
        execl("./process.out", "process.out", idText, runtimeText, shmText, (char *)NULL);
        perror("scheduler: execl process.out");
        _exit(1);
    }
    pcb->systemPid = pid;
}

static void on_resume(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->systemPid > 0)
        kill(pcb->systemPid, SIGCONT);
}

static void on_stop(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->systemPid > 0)
        kill(pcb->systemPid, SIGSTOP);
}

/* The process ran for one more second: publish its remaining time. */
static void on_execute(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->sharedRemainingTime)
        *pcb->sharedRemainingTime = pcb->runtime - pcb->executedTime;
}

/* Remaining time reached 0: wait until the process notifies us, then reap it. */
static void on_finish(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->systemPid <= 0)
        return;
    ProcessMessage message;
    for (;;) {
        ssize_t received = msgrcv(processQueueId, &message, MESSAGE_SIZE(ProcessMessage),
                                  MSG_PROCESS_FINISHED, 0);
        if (received == -1) {
            if (errno == EINTR)
                continue;
            perror("scheduler: msgrcv (process)");
            break;
        }
        if (message.processId == pcb->id)
            break;
    }
    waitpid(pcb->systemPid, NULL, 0);
    pcb->systemPid = -1;
    release_process_memory(pcb); /* delete the process data */
}

/* ------------------------------------------------------------------ */
/*                              Cleanup                                */
/* ------------------------------------------------------------------ */

static void kill_live_process(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->systemPid > 0) {
        kill(pcb->systemPid, SIGKILL);
        waitpid(pcb->systemPid, NULL, 0);
        pcb->systemPid = -1;
    }
    release_process_memory(pcb);
}

static void release_ipc(void)
{
    if (schedulerInitialized) {
        scheduler_for_each_live(&scheduler, kill_live_process, NULL);
        scheduler_destroy(&scheduler);
        schedulerInitialized = 0;
    }
    if (processQueueId != -1) {
        msgctl(processQueueId, IPC_RMID, NULL);
        processQueueId = -1;
    }
    if (logFile) {
        fclose(logFile);
        logFile = NULL;
    }
    destroyClk(false);
}

static void on_interrupt(int signum)
{
    (void)signum;
    release_ipc();
    exit(1);
}

/* ------------------------------------------------------------------ */
/*                                Main                                 */
/* ------------------------------------------------------------------ */

/* Growable buffer for the arrivals of one clock second. */
typedef struct {
    ProcessInfo *items;
    int count;
    int capacity;
} ArrivalBuffer;

static void arrival_buffer_add(ArrivalBuffer *buffer, const ProcessInfo *info)
{
    if (buffer->count == buffer->capacity) {
        int newCapacity = buffer->capacity ? buffer->capacity * 2 : 8;
        ProcessInfo *grown = realloc(buffer->items, newCapacity * sizeof(ProcessInfo));
        if (!grown) {
            fprintf(stderr, "scheduler: out of memory, dropping process %d\n", info->id);
            return;
        }
        buffer->items = grown;
        buffer->capacity = newCapacity;
    }
    buffer->items[buffer->count++] = *info;
}

/*
 * Receives all generator messages that belong to clock second `tick`.
 * Returns 1 when the generator reported that no more processes will come.
 */
static int receive_arrivals(int tick, ArrivalBuffer *buffer)
{
    GeneratorMessage message;
    for (;;) {
        if (msgrcv(generatorQueueId, &message, MESSAGE_SIZE(GeneratorMessage), 0, 0) == -1) {
            if (errno == EINTR)
                continue;
            perror("scheduler: msgrcv (generator)");
            return 1;
        }
        if (message.mtype == MSG_ARRIVAL) {
            arrival_buffer_add(buffer, &message.process);
        } else if (message.mtype == MSG_TICK_END && message.tick >= tick) {
            return message.isLast;
        }
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <algorithm 1-4> [quantum] [context_switch]\n", argv[0]);
        return 1;
    }
    Algorithm algorithm = (Algorithm)atoi(argv[1]);
    int quantum = argc > 2 ? atoi(argv[2]) : 1;
    int contextSwitch = (algorithm == ALGO_RR && argc > 3) ? atoi(argv[3]) : 1;
    if (algorithm < ALGO_SJF || algorithm > ALGO_HPF_AGING || quantum <= 0 || contextSwitch < 0) {
        fprintf(stderr, "scheduler: invalid parameters\n");
        return 1;
    }

    initClk();

    generatorQueueId = msgget(GENERATOR_MSGQ_KEY, 0666);
    processQueueId = msgget(PROCESS_MSGQ_KEY, IPC_CREAT | 0666);
    if (generatorQueueId == -1 || processQueueId == -1) {
        perror("scheduler: msgget");
        release_ipc();
        return 1;
    }

    logFile = fopen("scheduler.log", "w");
    if (!logFile) {
        perror("scheduler: scheduler.log");
        release_ipc();
        return 1;
    }

    SchedulerHooks hooks = {on_start, on_resume, on_stop, on_execute, on_finish, NULL};
    if (!scheduler_init(&scheduler, algorithm, quantum, contextSwitch, logFile, &hooks)) {
        fprintf(stderr, "scheduler: out of memory\n");
        release_ipc();
        return 1;
    }
    schedulerInitialized = 1;

    printf("Scheduler started: %s", algorithm_name(algorithm));
    if (algorithm == ALGO_RR)
        printf(" (quantum = %d, context switch = %d)", quantum, contextSwitch);
    printf("\n");
    fflush(stdout);

    ArrivalBuffer arrivals = {NULL, 0, 0};
    int simulationTime = 0;  /* last clock second fully processed */
    int generatorDone = 0;

    while (!generatorDone || !scheduler_is_idle(&scheduler)) {
        if (simulationTime >= getClk()) {
            usleep(1000); /* wait for the next clock tick */
            continue;
        }
        /* Process every elapsed second in order (catches up if we lagged). */
        simulationTime++;
        arrivals.count = 0;
        if (!generatorDone)
            generatorDone = receive_arrivals(simulationTime, &arrivals);
        scheduler_tick(&scheduler, simulationTime, arrivals.items, arrivals.count);
    }

    FILE *perfFile = fopen("scheduler.perf", "w");
    if (perfFile) {
        scheduler_write_perf(&scheduler, perfFile);
        fclose(perfFile);
    }
    scheduler_write_perf(&scheduler, stdout);
    printf("Scheduler finished at time %d\n", simulationTime);

    free(arrivals.items);
    release_ipc();
    return 0;
}
