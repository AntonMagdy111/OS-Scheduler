/*
 * resource.c
 * Resource table and waiter lists.
 */
#include <stdlib.h>
#include <string.h>

#include "resource.h"

/* djb2 string hash */
static unsigned hash_name(const char *name)
{
    unsigned hash = 5381;
    while (*name)
        hash = hash * 33u + (unsigned char)*name++;
    return hash % RESOURCE_BUCKETS;
}

void resource_table_init(ResourceTable *table)
{
    memset(table, 0, sizeof *table);
}

void resource_table_free(ResourceTable *table)
{
    for (int i = 0; i < RESOURCE_BUCKETS; i++) {
        Resource *current = table->buckets[i];
        while (current) {
            Resource *next = current->nextInBucket;
            free(current);
            current = next;
        }
        table->buckets[i] = NULL;
    }
}

Resource *resource_table_get(ResourceTable *table, const char *name)
{
    unsigned bucket = hash_name(name);
    for (Resource *r = table->buckets[bucket]; r; r = r->nextInBucket)
        if (strcmp(r->name, name) == 0)
            return r;

    Resource *created = calloc(1, sizeof(Resource));
    if (!created)
        return NULL;
    strncpy(created->name, name, MAX_RESOURCE_NAME - 1);
    created->nextInBucket = table->buckets[bucket];
    table->buckets[bucket] = created;
    return created;
}

/* A blocked process holds no resource, so it is ranked by its original priority. */
static int waiter_before(const PCB *a, const PCB *b)
{
    if (a->originalPriority != b->originalPriority)
        return a->originalPriority < b->originalPriority;
    return a->id < b->id;
}

void resource_add_waiter(Resource *resource, PCB *pcb)
{
    PCB **link = &resource->waiters;
    while (*link && waiter_before(*link, pcb))
        link = &(*link)->nextWaiter;
    pcb->nextWaiter = *link;
    *link = pcb;
}

PCB *resource_pop_waiter(Resource *resource)
{
    PCB *first = resource->waiters;
    if (first) {
        resource->waiters = first->nextWaiter;
        first->nextWaiter = NULL;
    }
    return first;
}

int resource_best_waiter_priority(const Resource *resource)
{
    return resource->waiters ? resource->waiters->originalPriority : NO_INHERITED_PRIORITY;
}
