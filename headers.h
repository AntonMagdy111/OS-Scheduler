/*
 * headers.h
 * Common Linux includes, IPC keys/message formats and the clock helpers.
 */
#ifndef HEADERS_H
#define HEADERS_H

#include <errno.h>
#include <stdio.h>      //if you don't use scanf/printf change this include
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "process_info.h"

typedef short bool;
#define true 1
#define false 0

/* ---------------------------- IPC keys ---------------------------- */
#define SHKEY 300              /* clock shared memory                   */
#define GENERATOR_MSGQ_KEY 400 /* process_generator -> scheduler        */
#define PROCESS_MSGQ_KEY 500   /* process -> scheduler (termination)    */

/* -------------------------- message types ------------------------- */
#define MSG_ARRIVAL 1          /* a process arrives at msg.tick         */
#define MSG_TICK_END 2         /* all arrivals of msg.tick were sent    */
#define MSG_PROCESS_FINISHED 3 /* a process notifies its termination    */

/* process_generator -> scheduler */
typedef struct {
    long mtype;
    int tick;       /* clock second this message belongs to             */
    int isLast;     /* MSG_TICK_END: no more processes will be sent     */
    ProcessInfo process;
} GeneratorMessage;

/* process -> scheduler */
typedef struct {
    long mtype;
    int processId;
    int systemPid;
} ProcessMessage;

#define MESSAGE_SIZE(type) (sizeof(type) - sizeof(long))

///==============================
//don't mess with this variable//
int * shmaddr;                 //
//===============================

static inline int getClk(void)
{
    return *(volatile int *)shmaddr;
}

/*
 * All processes call this function at the beginning to establish communication
 * between them and the clock module.
 * Again, remember that the clock is only emulation!
 */
static inline void initClk(void)
{
    int shmid = shmget(SHKEY, 4, 0444);
    while ((int)shmid == -1) {
        //Make sure that the clock exists
        printf("Wait! The clock not initialized yet!\n");
        sleep(1);
        shmid = shmget(SHKEY, 4, 0444);
    }
    shmaddr = (int *)shmat(shmid, (void *)0, 0);
}

/*
 * All processes call this function at the end to release the communication
 * resources between them and the clock module.
 * Again, Remember that the clock is only emulation!
 * Input: terminateAll: a flag to indicate whether that this is the end of simulation.
 *                      It terminates the whole system and releases resources.
 */
static inline void destroyClk(bool terminateAll)
{
    if (shmaddr) {
        shmdt(shmaddr);
        shmaddr = NULL;
    }
    if (terminateAll) {
        killpg(getpgrp(), SIGINT);
    }
}

#endif
