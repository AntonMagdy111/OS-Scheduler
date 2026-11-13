/*
 * process.c - a simulated CPU-bound process
 *
 * Started (fork + exec) by the scheduler as:   ./process.out <id> <shmid>
 *
 * Communication with the scheduler (the "two-way" arrows of the design):
 *   scheduler -> process : SIGSTOP / SIGCONT to stop and resume the process, and
 *                          a shared-memory integer holding the remaining time.
 *                          The scheduler is the only place that accounts for CPU
 *                          time, so the process just waits for it to reach zero.
 *   process -> scheduler : one message on DONE_QUEUE_KEY when it finishes.
 *                          The process notifies; the scheduler does NOT kill it.
 */
#include "headers.h"
#include "common.h"

int remainingtime;   /* local copy of the remaining time */

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <process id> <remaining-time shmid>\n", argv[0]);
        return 1;
    }
    int id    = atoi(argv[1]);
    int shmid = atoi(argv[2]);

    initClk();

    volatile int *sharedRemaining = (volatile int *)shmat(shmid, NULL, 0);
    if (sharedRemaining == (volatile int *)-1)
    {
        perror("process: cannot attach the remaining-time memory");
        return 1;
    }
    int doneQueue = msgget(DONE_QUEUE_KEY, 0666);
    if (doneQueue == -1)
    {
        perror("process: cannot open the termination queue");
        return 1;
    }

    /* "CPU bound": keep running until the scheduler's accounting says all of the
     * runtime has been consumed.  While the scheduler has the process stopped
     * (SIGSTOP) this loop simply does not execute.  A 1ms nap per iteration keeps
     * the simulation from starving the scheduler on a single-core machine. */
    remainingtime = *sharedRemaining;
    while (remainingtime > 0)
    {
        usleep(1000);
        remainingtime = *sharedRemaining;
    }

    /* Notify the scheduler that this process terminated. */
    DoneMsg done = { .mtype = id, .id = id };
    while (msgsnd(doneQueue, &done, DONE_MSG_SIZE, 0) == -1)
    {
        if (errno == EINTR)
            continue;
        perror("process: cannot notify the scheduler");
        break;
    }

    shmdt((const void *)sharedRemaining);
    destroyClk(false);
    return 0;
}
