/*
 * data_structures.c
 * Indexed min-heap and FIFO queue of PCBs.
 */
#include <stdlib.h>

#include "data_structures.h"

#define INITIAL_HEAP_CAPACITY 16

/* ------------------------------------------------------------------ */
/*                              Min-heap                               */
/* ------------------------------------------------------------------ */

int heap_init(MinHeap *heap, PcbLess less)
{
    heap->items = malloc(INITIAL_HEAP_CAPACITY * sizeof(PCB *));
    heap->size = 0;
    heap->capacity = heap->items ? INITIAL_HEAP_CAPACITY : 0;
    heap->less = less;
    return heap->items != NULL;
}

void heap_free(MinHeap *heap)
{
    free(heap->items);
    heap->items = NULL;
    heap->size = heap->capacity = 0;
}

static void heap_swap(MinHeap *heap, int i, int j)
{
    PCB *temp = heap->items[i];
    heap->items[i] = heap->items[j];
    heap->items[j] = temp;
    heap->items[i]->heapIndex = i;
    heap->items[j]->heapIndex = j;
}

static void sift_up(MinHeap *heap, int index)
{
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (!heap->less(heap->items[index], heap->items[parent]))
            break;
        heap_swap(heap, index, parent);
        index = parent;
    }
}

static void sift_down(MinHeap *heap, int index)
{
    for (;;) {
        int left = 2 * index + 1;
        int right = left + 1;
        int smallest = index;
        if (left < heap->size && heap->less(heap->items[left], heap->items[smallest]))
            smallest = left;
        if (right < heap->size && heap->less(heap->items[right], heap->items[smallest]))
            smallest = right;
        if (smallest == index)
            break;
        heap_swap(heap, index, smallest);
        index = smallest;
    }
}

int heap_push(MinHeap *heap, PCB *pcb)
{
    if (heap->size == heap->capacity) {
        int newCapacity = heap->capacity ? heap->capacity * 2 : INITIAL_HEAP_CAPACITY;
        PCB **grown = realloc(heap->items, newCapacity * sizeof(PCB *));
        if (!grown)
            return 0;
        heap->items = grown;
        heap->capacity = newCapacity;
    }
    heap->items[heap->size] = pcb;
    pcb->heapIndex = heap->size;
    heap->size++;
    sift_up(heap, pcb->heapIndex);
    return 1;
}

PCB *heap_peek(const MinHeap *heap)
{
    return heap->size > 0 ? heap->items[0] : NULL;
}

PCB *heap_pop(MinHeap *heap)
{
    if (heap->size == 0)
        return NULL;
    PCB *top = heap->items[0];
    heap->size--;
    if (heap->size > 0) {
        heap->items[0] = heap->items[heap->size];
        heap->items[0]->heapIndex = 0;
        sift_down(heap, 0);
    }
    top->heapIndex = -1;
    return top;
}

void heap_update(MinHeap *heap, PCB *pcb)
{
    if (pcb->heapIndex < 0 || pcb->heapIndex >= heap->size)
        return;
    sift_up(heap, pcb->heapIndex);
    sift_down(heap, pcb->heapIndex);
}

void heap_rebuild(MinHeap *heap)
{
    for (int i = heap->size / 2 - 1; i >= 0; i--)
        sift_down(heap, i);
}

/* ------------------------------------------------------------------ */
/*                             FIFO queue                              */
/* ------------------------------------------------------------------ */

void fifo_init(FifoQueue *queue)
{
    queue->head = queue->tail = NULL;
    queue->size = 0;
}

void fifo_push(FifoQueue *queue, PCB *pcb)
{
    pcb->nextInQueue = NULL;
    if (queue->tail)
        queue->tail->nextInQueue = pcb;
    else
        queue->head = pcb;
    queue->tail = pcb;
    queue->size++;
}

PCB *fifo_peek(const FifoQueue *queue)
{
    return queue->head;
}

PCB *fifo_pop(FifoQueue *queue)
{
    PCB *front = queue->head;
    if (!front)
        return NULL;
    queue->head = front->nextInQueue;
    if (!queue->head)
        queue->tail = NULL;
    front->nextInQueue = NULL;
    queue->size--;
    return front;
}
