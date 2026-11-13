/*
 * data_structures.h
 * Ready-queue containers used by the scheduler:
 *   - MinHeap  : indexed binary min-heap of PCB pointers (SJF, HPF).
 *                Each PCB stores its heap position, so a key change is
 *                fixed in O(log n) (needed for priority inheritance).
 *   - FifoQueue: intrusive singly linked FIFO of PCBs (Round Robin).
 */
#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include "pcb.h"

/* Returns non-zero when a must be served before b. */
typedef int (*PcbLess)(const PCB *a, const PCB *b);

typedef struct {
    PCB **items;
    int size;
    int capacity;
    PcbLess less;
} MinHeap;

int  heap_init(MinHeap *heap, PcbLess less);
void heap_free(MinHeap *heap);
int  heap_push(MinHeap *heap, PCB *pcb);   /* O(log n), returns 0 on failure */
PCB *heap_peek(const MinHeap *heap);       /* O(1)                            */
PCB *heap_pop(MinHeap *heap);              /* O(log n)                        */
void heap_update(MinHeap *heap, PCB *pcb); /* re-position after key change    */
void heap_rebuild(MinHeap *heap);          /* O(n) after many key changes     */

typedef struct {
    PCB *head;
    PCB *tail;
    int size;
} FifoQueue;

void fifo_init(FifoQueue *queue);
void fifo_push(FifoQueue *queue, PCB *pcb); /* O(1) */
PCB *fifo_peek(const FifoQueue *queue);     /* O(1) */
PCB *fifo_pop(FifoQueue *queue);            /* O(1) */

#endif
