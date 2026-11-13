/*
 * process_generator.c
 * Reads the input files, asks for the scheduling algorithm, creates the clock
 * and the scheduler, sends every process to the scheduler at its arrival
 * time and finally clears all IPC resources.
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
static ProcessInfo *processes = NULL;

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
    (void)signum;
    if (schedulerPid > 0) {
        kill(schedulerPid, SIGINT);
        waitpid(schedulerPid, NULL, 0);
        schedulerPid = -1;
    }
    if (clockPid > 0) {
        kill(clockPid, SIGINT);
        waitpid(clockPid, NULL, 0);
        clockPid = -1;
    }
    if (generatorQueueId != -1) {
        msgctl(generatorQueueId, IPC_RMID, NULL);
        generatorQueueId = -1;
    }
    remove_message_queue(PROCESS_MSGQ_KEY);
    destroyClk(false);
    free(processes);
    processes = NULL;
    exit(0);
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

static void send_message(GeneratorMessage *message)
{
    while (msgsnd(generatorQueueId, message, MESSAGE_SIZE(GeneratorMessage), 0) == -1) {
        if (errno != EINTR) {
            perror("process_generator: msgsnd");
            clearResources(1);
        }
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, clearResources);

    const char *processesPath = argc > 1 ? argv[1] : "processes.txt";
    const char *resourcesPath = argc > 2 ? argv[2] : "resources.txt";

    /* 1. Read the input files */
    int processCount = load_processes(processesPath, &processes);
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
    initClk();

    char algorithmText[16], quantumText[16], switchText[16];
    snprintf(algorithmText, sizeof algorithmText, "%d", algorithm);
    snprintf(quantumText, sizeof quantumText, "%d", quantum);
    snprintf(switchText, sizeof switchText, "%d", contextSwitch);
    char *schedulerArguments[] = {"scheduler.out", algorithmText, quantumText, switchText, NULL};
    schedulerPid = spawn("./scheduler.out", schedulerArguments);

    /* 5. Send each process to the scheduler when it arrives. After the
     *    arrivals of each second, a TICK_END message tells the scheduler that
     *    this second is complete. */
    int nextProcess = 0;
    for (int tick = 1;; tick++) {
        while (getClk() < tick)
            usleep(1000);

        GeneratorMessage message;
        memset(&message, 0, sizeof message);
        while (nextProcess < processCount && processes[nextProcess].arrival <= tick) {
            message.mtype = MSG_ARRIVAL;
            message.tick = tick;
            message.process = processes[nextProcess++];
            send_message(&message);
        }
        memset(&message, 0, sizeof message);
        message.mtype = MSG_TICK_END;
        message.tick = tick;
        message.isLast = nextProcess >= processCount;
        send_message(&message);
        if (message.isLast)
            break;
    }

    /* 6. Wait for the scheduler to finish, then clear clock & IPC resources */
    int status = 0;
    waitpid(schedulerPid, &status, 0);
    schedulerPid = -1;
    printf("Simulation finished. Output: scheduler.log, scheduler.perf\n");
    destroyClk(true);
    clearResources(0);
    return 0;
}
