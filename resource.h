/*
 * resource.h
 * Exclusive resources (HPF with priority inheritance).
 * Resources are looked up by name in a small chained hash table.
 * Each resource keeps its holder and a list of blocked waiters sorted by
 * (priority, id), so the next owner is always the list head (O(1) pick).
 */
#ifndef RESOURCE_H
#define RESOURCE_H

#include "pcb.h"
#include "process_info.h"

#define RESOURCE_BUCKETS 64

typedef struct Resource {
    char *name;                   /* heap-allocated resource name         */
    PCB *holder;                  /* process holding it, NULL if free     */
    PCB *waiters;                 /* blocked processes, best first        */
    struct Resource *nextInBucket;
} Resource;

typedef struct {
    Resource *buckets[RESOURCE_BUCKETS];
} ResourceTable;

void      resource_table_init(ResourceTable *table);
void      resource_table_free(ResourceTable *table);
Resource *resource_table_get(ResourceTable *table, const char *name); /* creates if missing */

void resource_add_waiter(Resource *resource, PCB *pcb);
PCB *resource_pop_waiter(Resource *resource);
int  resource_best_waiter_priority(const Resource *resource); /* NO_INHERITED_PRIORITY if none */

#endif
