/*
 * scheduler.c
 * The scheduler process: connects the scheduling engine (sched_core.c) to
 * the real system.
 *
 *  - Receives arriving processes (and streamed resource requests) from the
 *    process generator via System V message queue.
 *  - Forks process.out on first dispatch, SIGSTOP/SIGCONT to switch.
 *  - Keeps each process's remaining time in a private shared memory segment.
 *  - Waits for the process's own termination message without hanging if a
 *    worker exits unexpectedly, and cleans up all IPC on failure or exit.
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
static int fatalError = 0;

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
        fatalError = 1;
        return;
    }
    int *remaining = (int *)shmat(pcb->sharedMemoryId, NULL, 0);
    if ((long)remaining == -1) {
        perror("scheduler: shmat");
        release_process_memory(pcb);
        fatalError = 1;
        return;
    }
    *remaining = pcb->runtime - pcb->executedTime;
    pcb->sharedRemainingTime = remaining;

    pid_t pid = fork();
    if (pid == -1) {
        perror("scheduler: fork");
        release_process_memory(pcb);
        fatalError = 1;
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

/*
 * Remaining time reached 0: wait until the worker sends its termination
 * notice, while polling waitpid(WNOHANG) so an unexpected worker crash or
 * exec failure can never hang the scheduler forever.
 */
static void on_finish(PCB *pcb, void *context)
{
    (void)context;
    if (pcb->systemPid <= 0) {
        fprintf(stderr, "scheduler: process %d has no live worker\n", pcb->id);
        fatalError = 1;
        release_process_memory(pcb);
        return;
    }

    ProcessMessage message;
    long finishType = (long)pcb->id + 1; /* > 0 even if a test uses id == 0 */
    int reaped = 0;
    for (;;) {
        ssize_t r = msgrcv(processQueueId, &message, MESSAGE_SIZE(ProcessMessage),
                           finishType, IPC_NOWAIT);
        if (r != -1)
            break;
        if (errno == EINTR)
            continue;
        if (errno != ENOMSG) {
            perror("scheduler: msgrcv (process)");
            fatalError = 1;
            break;
        }
        int wstatus = 0;
        pid_t wp = waitpid(pcb->systemPid, &wstatus, WNOHANG);
        if (wp == pcb->systemPid) {
            /* One final non-blocking check in case the notice arrived right before exit. */
            if (msgrcv(processQueueId, &message, MESSAGE_SIZE(ProcessMessage),
                       finishType, IPC_NOWAIT) == -1) {
                fprintf(stderr, "scheduler: worker for process %d exited before notifying completion\n",
                        pcb->id);
                fatalError = 1;
            }
            reaped = 1;
            break;
        }
        if (wp == -1 && errno != EINTR) {
            perror("scheduler: waitpid");
            fatalError = 1;
            break;
        }
        usleep(1000);
    }

    if (!reaped) {
        while (waitpid(pcb->systemPid, NULL, 0) == -1 && errno == EINTR) {
        }
    }
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
        while (waitpid(pcb->systemPid, NULL, 0) == -1 && errno == EINTR) {
        }
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

static void arrival_buffer_clear(ArrivalBuffer *buffer)
{
    for (int i = 0; i < buffer->count; i++)
        free_process_resources(&buffer->items[i]);
    buffer->count = 0;
}

static void arrival_buffer_free(ArrivalBuffer *buffer)
{
    arrival_buffer_clear(buffer);
    free(buffer->items);
    buffer->items = NULL;
    buffer->capacity = 0;
}

static int arrival_buffer_add(ArrivalBuffer *buffer, ProcessInfo *info)
{
    if (buffer->count == buffer->capacity) {
        int newCapacity = buffer->capacity ? buffer->capacity * 2 : 8;
        ProcessInfo *grown = realloc(buffer->items, (size_t)newCapacity * sizeof(ProcessInfo));
        if (!grown) {
            fprintf(stderr, "scheduler: out of memory, dropping process %d\n", info->id);
            free_process_resources(info);
            return 0;
        }
        buffer->items = grown;
        buffer->capacity = newCapacity;
    }
    buffer->items[buffer->count++] = *info; /* takes ownership of info->resources */
    memset(info, 0, sizeof *info);
    return 1;
}

/* Appends `chunk` to a dynamically growing string buffer. */
static int append_name_chunk(char **nameBuf, size_t *nameLen, size_t *nameCap, const char *chunk)
{
    size_t add = strlen(chunk);
    if (*nameLen + add + 1 > *nameCap) {
        size_t newCap = *nameCap ? *nameCap * 2 : 64;
        while (newCap < *nameLen + add + 1)
            newCap *= 2;
        char *grown = realloc(*nameBuf, newCap);
        if (!grown)
            return 0;
        *nameBuf = grown;
        *nameCap = newCap;
    }
    memcpy(*nameBuf + *nameLen, chunk, add + 1);
    *nameLen += add;
    return 1;
}

/*
 * Receives all generator messages that belong to clock second `tick`.
 * Returns 1 when the generator reported that no more processes will come.
 */
static int receive_arrivals(int tick, ArrivalBuffer *buffer)
{
    GeneratorMessage message;
    ProcessInfo pending;
    memset(&pending, 0, sizeof pending);
    char *nameBuf = NULL;
    size_t nameLen = 0, nameCap = 0;

    for (;;) {
        if (msgrcv(generatorQueueId, &message, MESSAGE_SIZE(GeneratorMessage), 0, 0) == -1) {
            if (errno == EINTR)
                continue;
            perror("scheduler: msgrcv (generator)");
            free(nameBuf);
            free_process_resources(&pending);
            fatalError = 1;
            return 1;
        }
        if (message.mtype == MSG_RESOURCE) {
            message.resourceChunk[IPC_NAME_CHUNK - 1] = '\0';
            if (!append_name_chunk(&nameBuf, &nameLen, &nameCap, message.resourceChunk)) {
                fprintf(stderr, "scheduler: out of memory assembling resource name\n");
                free(nameBuf);
                free_process_resources(&pending);
                fatalError = 1;
                return 1;
            }
            if (!message.moreChunks) {
                if (!process_add_resource(&pending, nameBuf, message.captureTime, message.releaseTime)) {
                    fprintf(stderr, "scheduler: out of memory storing resource request\n");
                    free(nameBuf);
                    free_process_resources(&pending);
                    fatalError = 1;
                    return 1;
                }
                nameLen = 0;
                if (nameBuf)
                    nameBuf[0] = '\0';
            }
        } else if (message.mtype == MSG_ARRIVAL) {
            pending.id = message.id;
            pending.arrival = message.arrival;
            pending.runtime = message.runtime;
            pending.priority = message.priority;
            if (!arrival_buffer_add(buffer, &pending))
                fatalError = 1;
        } else if (message.mtype == MSG_TICK_END && message.tick >= tick) {
            free(nameBuf);
            free_process_resources(&pending);
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

    while (shmget(SHKEY, 4, 0444) == -1)
        usleep(5000);
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

    while (!fatalError && (!generatorDone || !scheduler_is_idle(&scheduler))) {
        if (simulationTime >= getClk()) {
            usleep(1000); /* wait for the next clock tick */
            continue;
        }
        /* Process every elapsed second in order (catches up if we lagged). */
        simulationTime++;
        arrival_buffer_clear(&arrivals);
        if (!generatorDone)
            generatorDone = receive_arrivals(simulationTime, &arrivals);
        if (!fatalError)
            scheduler_tick(&scheduler, simulationTime, arrivals.items, arrivals.count);
        arrival_buffer_clear(&arrivals);
    }

    arrival_buffer_free(&arrivals);

    if (fatalError) {
        fprintf(stderr, "scheduler: aborting due to fatal IPC or process error\n");
        release_ipc();
        return 1;
    }

    FILE *perfFile = fopen("scheduler.perf", "w");
    if (!perfFile) {
        perror("scheduler: scheduler.perf");
        release_ipc();
        return 1;
    }
    scheduler_write_perf(&scheduler, perfFile);
    if (fclose(perfFile) != 0) {
        perror("scheduler: closing scheduler.perf");
        release_ipc();
        return 1;
    }

    scheduler_write_perf(&scheduler, stdout);
    printf("Scheduler finished at time %d\n", simulationTime);

    release_ipc();
    return 0;
}
