/*
 * common.h - definitions shared by process_generator.c, scheduler.c and process.c
 *
 * NOTE: include "headers.h" BEFORE this file (headers.h has no include guard and
 * defines the clock helpers, so it must be included exactly once per .c file).
 */
#ifndef COMMON_H
#define COMMON_H

#include <string.h>
#include <errno.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Algorithms                                                         */
/* ------------------------------------------------------------------ */
#define ALGO_SJF           1   /* Shortest Job First (non-preemptive)               */
#define ALGO_RR            2   /* Round Robin                                       */
#define ALGO_HPF           3   /* Preemptive HPF + priority inheritance             */
#define ALGO_HPF_DEMOTION  4   /* Bonus: HPF + inheritance + priority aging/demotion */

#define MAX_PRIORITY       10  /* priorities range over 0 (highest) .. 10 (lowest)  */
#define DEFAULT_SWITCH_TIME 1  /* context-switch time (fixed for SJF/HPF)           */

/* ------------------------------------------------------------------ */
/*  IPC keys (System V).  SHKEY (300) is already used by the clock.     */
/* ------------------------------------------------------------------ */
#define GEN_QUEUE_KEY   301    /* process_generator -> scheduler                    */
#define DONE_QUEUE_KEY  302    /* process           -> scheduler (termination)     */

#define MAX_RES_NAME    32

/* ------------------------------------------------------------------ */
/*  Generator -> scheduler messages                                    */
/*                                                                     */
/*  Every message of clock second T carries mtype == T, so the scheduler*/
/*  can receive exactly "the arrivals of second T".  Within one second  */
/*  the generator sends, in this order:                                 */
/*      [MSG_RESOURCE ...]  MSG_PROCESS   (once per arriving process)    */
/*      MSG_TICK_DONE                     (always, closes the second)    */
/*  The queue is FIFO per mtype, so the resource messages that precede  */
/*  a MSG_PROCESS belong to that process.                               */
/*  MSG_TICK_DONE.last == 1 means "no more processes will ever arrive". */
/* ------------------------------------------------------------------ */
#define MSG_RESOURCE   1
#define MSG_PROCESS    2
#define MSG_TICK_DONE  3

typedef struct
{
    long mtype;                    /* = clock second this message belongs to */
    int  kind;                     /* MSG_RESOURCE / MSG_PROCESS / MSG_TICK_DONE */
    /* MSG_PROCESS */
    int  id, arrival, runtime, priority;
    /* MSG_RESOURCE (belongs to the MSG_PROCESS that follows) */
    char resource[MAX_RES_NAME];
    int  capture, release;
    /* MSG_TICK_DONE */
    int  last;
} GenMsg;
#define GEN_MSG_SIZE (sizeof(GenMsg) - sizeof(long))

/* ------------------------------------------------------------------ */
/*  Process -> scheduler termination notice (mtype == process id)       */
/* ------------------------------------------------------------------ */
typedef struct
{
    long mtype;
    int  id;
} DoneMsg;
#define DONE_MSG_SIZE (sizeof(DoneMsg) - sizeof(long))

/* One line of resources.txt: "process <pid> holds <resource> from executed time capture to release" */
typedef struct
{
    char name[MAX_RES_NAME];
    int  capture;
    int  release;
} ResourceUse;

#endif /* COMMON_H */
