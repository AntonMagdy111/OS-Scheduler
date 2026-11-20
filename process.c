/*
 * process.c
 * A simulated CPU-bound process.
 *
 * The scheduler forks/execs it with: <id> <runtime> <shm id>.
 * The shared memory holds this process's remaining time; the scheduler
 * updates it after every second the process spends on the CPU and uses
 * SIGSTOP/SIGCONT to take it off / put it back on the CPU.
 * The process busy-loops (CPU-bound) until its remaining time reaches 0, then
 * notifies the scheduler through a message queue and terminates by itself.
 */
#include "headers.h"

int remainingtime;

int main(int argc, char *argv[])
{
    initClk();

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <id> <runtime> <shm id>\n", argv[0]);
        destroyClk(false);
        return 1;
    }
    int processId = atoi(argv[1]);
    int sharedMemoryId = atoi(argv[3]);

    volatile int *sharedRemaining = (volatile int *)shmat(sharedMemoryId, NULL, 0);
    if ((long)sharedRemaining == -1) {
        perror("process: shmat");
        destroyClk(false);
        return 1;
    }
    int queueId = msgget(PROCESS_MSGQ_KEY, 0666);
    if (queueId == -1) {
        perror("process: msgget");
        shmdt((const void *)sharedRemaining);
        destroyClk(false);
        return 1;
    }

    /* CPU-bound work: run while there is remaining time (1ms sleep avoids
     * starving clk.out and scheduler.out on a 1-CPU machine). */
    while ((remainingtime = *sharedRemaining) > 0) {
        usleep(1000);
    }

    /* notify the scheduler of the termination (mtype = id + 1 ensures mtype > 0 even if id == 0) */
    ProcessMessage message;
    message.mtype = (long)processId + 1;
    message.processId = processId;
    message.systemPid = getpid();
    while (msgsnd(queueId, &message, MESSAGE_SIZE(ProcessMessage), 0) == -1 && errno == EINTR) {
    }

    shmdt((const void *)sharedRemaining);
    destroyClk(false);
    return 0;
}
