/*
 * pcb.h - scheduler data structures
 *
 *   PCB       process control block (one per live process)
 *   Resource  exclusive resource: current holder + list of blocked waiters
 *   Heap      binary min-heap of PCBs with O(log n) push / pop / re-key
 *             (ready queue of SJF and HPF)
 *   Fifo      linked-list queue, O(1) push / pop (ready queue of Round Robin)
 *
 * Include after "headers.h" and "common.h".
 */
#ifndef PCB_H
#define PCB_H

#include <stdlib.h>

typedef enum { PS_READY, PS_RUNNING, PS_BLOCKED } ProcState;

struct PCB;

/* An exclusive resource (resources.txt).  At most one holder; blocked
 * processes wait in an unordered array (the best one is picked on release). */
typedef struct Resource
{
    char        name[MAX_RES_NAME];
    struct PCB *holder;           /* NULL when free */
    struct PCB **waiters;         /* processes blocked on this resource */
    int         numWaiters;
    int         capWaiters;
} Resource;

/* One resource usage of a process, resolved to its Resource object. */
typedef struct
{
    Resource *res;
    int       capture;            /* executed time at which it is requested */
    int       release;            /* executed time at which it is released  */
} Usage;

typedef struct PCB
{
    /* ---- input data ---- */
    int id, arrival, runtime, priority;   /* priority: original, 0 = highest */

    /* ---- scheduling state ---- */
    ProcState state;
    int  curPriority;       /* priority in force = min(original, inherited)     */
    int  executed;          /* seconds of CPU received so far                   */
    int  remaining;         /* runtime - executed                               */
    int  readySince;        /* start of the current consecutive Ready period    */
    bool started;           /* has the process been forked yet?                 */

    /* ---- resources ---- */
    Usage *uses;            /* sorted by capture time                           */
    int    numUses;
    int    nextUse;         /* index of the next usage to capture               */
    int    heldUse;         /* index of the usage currently held, -1 if none    */
    Resource *waitingOn;    /* resource this process is blocked on (or NULL)    */

    /* ---- OS-level handles ---- */
    pid_t pid;
    int   shmid;            /* shared memory holding the remaining time         */
    int  *remainingShm;     /* scheduler's mapping of that memory               */

    /* ---- container bookkeeping ---- */
    int   heapPos;          /* index inside the heap, -1 when not in the heap   */
    struct PCB *nextInQueue;/* link used by Fifo                                */
    struct PCB *prevAlive, *nextAlive;    /* list of live processes (cleanup)   */
} PCB;

/* ================================================================== */
/*  Binary heap                                                        */
/* ================================================================== */

/* returns non-zero when a must be served before b */
typedef int (*PrecedesFn)(const PCB *a, const PCB *b);

typedef struct
{
    PCB      **items;
    int        size, capacity;
    PrecedesFn precedes;
} Heap;

static void heapInit(Heap *h, PrecedesFn precedes)
{
    h->items = NULL;
    h->size = h->capacity = 0;
    h->precedes = precedes;
}

static void heapPlace(Heap *h, int i, PCB *p)
{
    h->items[i] = p;
    p->heapPos = i;
}

static void heapSiftUp(Heap *h, int i)
{
    PCB *p = h->items[i];
    while (i > 0)
    {
        int parent = (i - 1) / 2;
        if (!h->precedes(p, h->items[parent]))
            break;
        heapPlace(h, i, h->items[parent]);
        i = parent;
    }
    heapPlace(h, i, p);
}

static void heapSiftDown(Heap *h, int i)
{
    PCB *p = h->items[i];
    for (;;)
    {
        int child = 2 * i + 1;
        if (child >= h->size)
            break;
        if (child + 1 < h->size && h->precedes(h->items[child + 1], h->items[child]))
            child++;
        if (!h->precedes(h->items[child], p))
            break;
        heapPlace(h, i, h->items[child]);
        i = child;
    }
    heapPlace(h, i, p);
}

static void heapPush(Heap *h, PCB *p)
{
    if (h->size == h->capacity)
    {
        h->capacity = h->capacity ? 2 * h->capacity : 16;
        h->items = realloc(h->items, h->capacity * sizeof(PCB *));
        if (h->items == NULL)
        {
            perror("heapPush: out of memory");
            exit(1);
        }
    }
    h->size++;
    heapPlace(h, h->size - 1, p);
    heapSiftUp(h, h->size - 1);
}

static PCB *heapPeek(const Heap *h)
{
    return h->size ? h->items[0] : NULL;
}

static PCB *heapPop(Heap *h)
{
    if (h->size == 0)
        return NULL;
    PCB *top = h->items[0];
    PCB *last = h->items[--h->size];
    top->heapPos = -1;
    if (h->size > 0)
    {
        heapPlace(h, 0, last);
        heapSiftDown(h, 0);
    }
    return top;
}

/* The key of p changed (p is inside the heap): restore the heap property. */
static void heapFix(Heap *h, PCB *p)
{
    if (p->heapPos < 0)
        return;
    heapSiftUp(h, p->heapPos);
    heapSiftDown(h, p->heapPos);
}

/* Many keys changed at once (priority aging): O(n) re-heapify. */
static void heapRebuild(Heap *h)
{
    for (int i = h->size / 2 - 1; i >= 0; i--)
        heapSiftDown(h, i);
}

/* ================================================================== */
/*  FIFO queue                                                         */
/* ================================================================== */
typedef struct
{
    PCB *head, *tail;
    int  size;
} Fifo;

static void fifoPush(Fifo *q, PCB *p)
{
    p->nextInQueue = NULL;
    if (q->tail)
        q->tail->nextInQueue = p;
    else
        q->head = p;
    q->tail = p;
    q->size++;
}

static PCB *fifoPop(Fifo *q)
{
    PCB *p = q->head;
    if (p == NULL)
        return NULL;
    q->head = p->nextInQueue;
    if (q->head == NULL)
        q->tail = NULL;
    p->nextInQueue = NULL;
    q->size--;
    return p;
}

#endif /* PCB_H */
