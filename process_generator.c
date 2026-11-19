/*
 * process_generator.c
 * Reads the input files, asks for the scheduling algorithm, creates the clock
 * and the scheduler, sends every process (and its resource requests) to the
 * scheduler at its arrival time, checks the scheduler's exit status, and
 * clears all IPC resources cleanly without affecting external processes.
 *
 * Usage:
 *   process_generator.out [processes.txt] [resources.txt] [algorithm] [quantum] [context_switch]
 * Missing algorithm parameters are asked interactively.
 */
#include "headers.h"

#define REQUIRED (-1) /* read_int: no default value */

static int generatorQueueId = -1;
static pid_t clockPid = -1;
static pid_t schedulerPid = -1;
static int schedulerReaped = 0;
static int schedulerExitStatus = 0;
static ProcessInfo *processes = NULL;
static int processCount = 0;
static volatile sig_atomic_t cleaningUp = 0;

void clearResources(int signum);

static void remove_message_queue(key_t key)
{
    int id = msgget(key, 0);
    if (id != -1)
        msgctl(id, IPC_RMID, NULL);
}

static void remove_stale_clock(void)
{
    int id = shmget(SHKEY, 4, 0);
    if (id != -1)
        shmctl(id, IPC_RMID, NULL);
}

/* Clear all IPC resources and child processes, then exit. */
void clearResources(int signum)
{
    if (cleaningUp)
        _exit(signum == 0 ? 0 : 1);
    cleaningUp = 1;

    if (schedulerPid > 0) {
        kill(schedulerPid, SIGINT);
        while (waitpid(schedulerPid, NULL, 0) == -1 && errno == EINTR) {
        }
        schedulerPid = -1;
    }
    if (clockPid > 0) {
        /* Always signal clockPid directly so single-PID SIGINT never hangs,
         * and avoid killpg() so caller scripts / shells are never signaled. */
        kill(clockPid, SIGINT);
        while (waitpid(clockPid, NULL, 0) == -1 && errno == EINTR) {
        }
        clockPid = -1;
    }
    if (generatorQueueId != -1) {
        msgctl(generatorQueueId, IPC_RMID, NULL);
        generatorQueueId = -1;
    }
    remove_message_queue(PROCESS_MSGQ_KEY);
    destroyClk(false);
    free_processes(processes, processCount);
    processes = NULL;
    processCount = 0;
    exit(signum == 0 ? 0 : 1);
}

/* Returns 1 if scheduler is still running, 0 if it already exited. */
static int scheduler_is_alive(void)
{
    if (schedulerPid <= 0)
        return 0;
    int status = 0;
    pid_t wp = waitpid(schedulerPid, &status, WNOHANG);
    if (wp == schedulerPid) {
        schedulerPid = -1;
        schedulerReaped = 1;
        schedulerExitStatus = status;
        return 0;
    }
    if (wp == -1 && errno != EINTR) {
        schedulerPid = -1;
        schedulerReaped = 1;
        schedulerExitStatus = 1 << 8;
        return 0;
    }
    return 1;
}

/* Reads an integer in [minValue, maxValue]; an empty line selects the default. */
static int read_int(const char *prompt, int defaultValue, int minValue, int maxValue)
{
    char line[64];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            if (defaultValue != REQUIRED)
                return defaultValue;
            fprintf(stderr, "\nNo input given.\n");
            exit(1);
        }
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if ((*cursor == '\n' || *cursor == '\0') && defaultValue != REQUIRED)
            return defaultValue;

        char *end;
        long value = strtol(cursor, &end, 10);
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
            end++;
        if (end != cursor && *end == '\0' && value >= minValue && value <= maxValue)
            return (int)value;
        printf("Invalid input, please enter a number between %d and %d.\n", minValue, maxValue);
    }
}

static pid_t spawn(const char *path, char *const arguments[])
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("process_generator: fork");
        clearResources(1);
    }
    if (pid == 0) {
        execv(path, arguments);
        perror("process_generator: execv");
        _exit(1);
    }
    return pid;
}

/* Sends one message; returns 1 on success, 0 if scheduler died or IPC failed. */
static int send_message(GeneratorMessage *message)
{
    for (;;) {
        if (msgsnd(generatorQueueId, message, MESSAGE_SIZE(GeneratorMessage), IPC_NOWAIT) == 0)
            return 1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN) {
            if (!scheduler_is_alive())
                return 0;
            usleep(1000);
            continue;
        }
        perror("process_generator: msgsnd");
        return 0;
    }
}

/* Streams all resource requests of `p` (splitting long names if needed), then `p`. */
static int send_process(int tick, const ProcessInfo *p)
{
    GeneratorMessage message;
    for (int i = 0; i < p->resourceCount; i++) {
        const ResourceRequest *req = &p->resources[i];
        const char *name = req->name ? req->name : "";
        size_t len = strlen(name);
        size_t offset = 0;
        do {
            memset(&message, 0, sizeof message);
            message.mtype = MSG_RESOURCE;
            message.tick = tick;
            message.captureTime = req->captureTime;
            message.releaseTime = req->releaseTime;
            size_t chunk = len - offset;
            if (chunk >= IPC_NAME_CHUNK) {
                chunk = IPC_NAME_CHUNK - 1;
                message.moreChunks = 1;
            } else {
                message.moreChunks = 0;
            }
            memcpy(message.resourceChunk, name + offset, chunk);
            message.resourceChunk[chunk] = '\0';
            if (!send_message(&message))
                return 0;
            offset += chunk;
        } while (message.moreChunks);
    }

    memset(&message, 0, sizeof message);
    message.mtype = MSG_ARRIVAL;
    message.tick = tick;
    message.id = p->id;
    message.arrival = p->arrival;
    message.runtime = p->runtime;
    message.priority = p->priority;
    return send_message(&message);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, clearResources);
    signal(SIGTERM, clearResources);

    const char *processesPath = argc > 1 ? argv[1] : "processes.txt";
    const char *resourcesPath = argc > 2 ? argv[2] : "resources.txt";

    /* 1. Read the input files */
    processCount = load_processes(processesPath, &processes);
    if (processCount < 0) {
        fprintf(stderr, "Cannot open %s\n", processesPath);
        return 1;
    }
    if (load_resources(resourcesPath, processes, processCount) < 0)
        printf("Note: %s not found, no resources will be used.\n", resourcesPath);
    printf("Loaded %d processes from %s\n", processCount, processesPath);

    /* 2. Ask for the scheduling algorithm and its parameters */
    int algorithm, quantum = 1, contextSwitch = 1;
    if (argc > 3) {
        algorithm = atoi(argv[3]);
        quantum = argc > 4 ? atoi(argv[4]) : 1;
        contextSwitch = argc > 5 ? atoi(argv[5]) : 1;
        if (algorithm < 1 || algorithm > 4 || quantum <= 0 || contextSwitch < 0) {
            fprintf(stderr, "Invalid algorithm parameters\n");
            free_processes(processes, processCount);
            return 1;
        }
    } else {
        printf("Choose a scheduling algorithm:\n"
               "  1. Shortest Job First (SJF)\n"
               "  2. Round Robin (RR)\n"
               "  3. Preemptive HPF with Priority Inheritance\n"
               "  4. HPF with Priority Demotion (bonus)\n");
        algorithm = read_int("Algorithm: ", REQUIRED, 1, 4);
        if (algorithm == 2) {
            quantum = read_int("Quantum: ", REQUIRED, 1, 1000000);
            contextSwitch = read_int("Context switch time [1]: ", 1, 0, 1000000);
        }
    }
    if (algorithm != 2)
        contextSwitch = 1; /* fixed at 1 second for SJF and HPF */

    /* 3. Create the IPC resources (removing leftovers of a crashed run) */
    remove_message_queue(GENERATOR_MSGQ_KEY);
    remove_message_queue(PROCESS_MSGQ_KEY);
    remove_stale_clock();
    generatorQueueId = msgget(GENERATOR_MSGQ_KEY, IPC_CREAT | 0666);
    if (generatorQueueId == -1) {
        perror("process_generator: msgget");
        clearResources(1);
    }

    /* 4. Initiate and create the clock and scheduler processes */
    char *clockArguments[] = {"clk.out", NULL};
    clockPid = spawn("./clk.out", clockArguments);

    char algorithmText[16], quantumText[16], switchText[16];
    snprintf(algorithmText, sizeof algorithmText, "%d", algorithm);
    snprintf(quantumText, sizeof quantumText, "%d", quantum);
    snprintf(switchText, sizeof switchText, "%d", contextSwitch);
    char *schedulerArguments[] = {"scheduler.out", algorithmText, quantumText, switchText, NULL};
    schedulerPid = spawn("./scheduler.out", schedulerArguments);

    while (shmget(SHKEY, 4, 0444) == -1) {
        if (!scheduler_is_alive())
            clearResources(1);
        usleep(5000);
    }
    initClk();

    /* 5. Send each process to the scheduler when it arrives. After the
     *    arrivals of each second, a TICK_END message tells the scheduler that
     *    this second is complete. */
    int nextProcess = 0;
    int sendOk = 1;
    for (int tick = 1; sendOk; tick++) {
        while (getClk() < tick) {
            if (!scheduler_is_alive()) {
                sendOk = 0;
                break;
            }
            usleep(1000);
        }
        if (!sendOk)
            break;

        while (nextProcess < processCount && processes[nextProcess].arrival <= tick) {
            if (!send_process(tick, &processes[nextProcess++])) {
                sendOk = 0;
                break;
            }
        }
        if (!sendOk)
            break;

        GeneratorMessage message;
        memset(&message, 0, sizeof message);
        message.mtype = MSG_TICK_END;
        message.tick = tick;
        message.isLast = nextProcess >= processCount;
        if (!send_message(&message)) {
            sendOk = 0;
            break;
        }
        if (message.isLast)
            break;
    }

    /* 6. Wait for the scheduler to finish and verify its exit status */
    if (!schedulerReaped && schedulerPid > 0) {
        while (waitpid(schedulerPid, &schedulerExitStatus, 0) == -1 && errno == EINTR) {
        }
        schedulerPid = -1;
        schedulerReaped = 1;
    }

    int schedulerOk = sendOk && WIFEXITED(schedulerExitStatus) &&
                      WEXITSTATUS(schedulerExitStatus) == 0;
    if (!schedulerOk) {
        fprintf(stderr, "process_generator: simulation failed (scheduler exited abnormally)\n");
        clearResources(1);
    }

    printf("Simulation finished. Output: scheduler.log, scheduler.perf\n");
    clearResources(0);
    return 0;
}
