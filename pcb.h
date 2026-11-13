/*
 * pcb.h
 * Process Control Block: everything the scheduler knows about a process.
 */
#ifndef PCB_H
#define PCB_H

#define NO_INHERITED_PRIORITY 1000000 /* "no inheritance active" marker */

typedef enum {
    STATE_READY,    /* arrived, waiting for the CPU            */
    STATE_RUNNING,  /* currently on the CPU                    */
    STATE_BLOCKED,  /* waiting for a resource held by another  */
    STATE_FINISHED  /* done (PCB is about to be deleted)       */
} ProcessState;

struct Resource;

/* One resource usage interval of a process (resolved to a Resource object). */
typedef struct {
    struct Resource *resource;
    int captureTime; /* executed time at which the resource is requested */
    int releaseTime; /* executed time at which it is released            */
} ResourceUse;

typedef struct PCB {
    /* ---- static information (from processes.txt) ---- */
    int id;
    int arrivalTime;
    int runtime;
    int originalPriority;

    /* ---- dynamic state ---- */
    ProcessState state;
    int executedTime;      /* seconds actually executed on the CPU        */
    int hasStarted;        /* 0 until the first dispatch ("started")      */
    int readySince;        /* time it last entered Ready (aging counter)  */
    int priorityKey;       /* cached comparison priority in the ready heap*/
    int inheritedPriority; /* best priority of processes blocked on us    */

    /* ---- resources (HPF only) ---- */
    ResourceUse *resourceUses; /* sorted by capture time               */
    int resourceUseCount;
    int nextResourceUse;       /* index of the next interval to capture */
    struct Resource *heldResource;
    int heldReleaseTime;
    struct Resource *waitingFor;
    int pendingReleaseTime;    /* release time of the awaited resource  */

    /* ---- container bookkeeping ---- */
    int heapIndex;              /* position in the ready heap, -1 if none */
    struct PCB *nextInQueue;    /* RR ready FIFO link                     */
    struct PCB *nextWaiter;     /* resource waiters list link             */
    struct PCB *prevLive;       /* list of all live PCBs                  */
    struct PCB *nextLive;

    /* ---- OS handles (filled by the IPC layer) ---- */
    int systemPid;                     /* pid of the forked process.out  */
    int sharedMemoryId;                /* shm holding remaining time      */
    volatile int *sharedRemainingTime; /* attached address of that shm    */
} PCB;

#endif
