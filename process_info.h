/*
 * process_info.h
 * Plain description of a process as read from the input files.
 * This header is portable (no Linux-specific includes) so it is shared by
 * the process generator, the scheduler and the offline simulator.
 */
#ifndef PROCESS_INFO_H
#define PROCESS_INFO_H

#define MAX_RESOURCE_NAME 16          /* max length of a resource id (incl. '\0') */
#define MAX_RESOURCES_PER_PROCESS 16  /* max resources.txt lines per process      */

/* One line of resources.txt (already attached to its process). */
typedef struct {
    char name[MAX_RESOURCE_NAME]; /* resource id, e.g. "x"                     */
    int captureTime;              /* executed time at which it is captured      */
    int releaseTime;              /* executed time at which it is released      */
} ResourceRequest;

/* One line of processes.txt plus the resources it uses. */
typedef struct {
    int id;
    int arrival;
    int runtime;
    int priority;
    int resourceCount;
    ResourceRequest resources[MAX_RESOURCES_PER_PROCESS];
} ProcessInfo;

/*
 * Reads processes.txt. Allocates *outList (caller frees).
 * Returns the number of processes, or -1 if the file cannot be opened.
 * The list is sorted by (arrival, id).
 */
int load_processes(const char *path, ProcessInfo **outList);

/*
 * Reads resources.txt and attaches each entry to its process.
 * Returns the number of accepted entries, or -1 if the file cannot be opened.
 */
int load_resources(const char *path, ProcessInfo *list, int count);

/*
 * Sorts a process's resource requests by capture time and drops entries that
 * violate the rules (1 <= capture < release <= runtime, no overlapping).
 */
void normalize_resource_requests(ProcessInfo *process);

#endif
