/*
 * process_info.h
 * Plain description of a process as read from the input files.
 * Portable header (no Linux-specific includes) shared by the process
 * generator, the scheduler, and the offline simulator.
 *
 * Resource names and per-process resource lists are dynamically allocated so
 * there are no fixed limits on name length or number of requests per process.
 */
#ifndef PROCESS_INFO_H
#define PROCESS_INFO_H

/* One line of resources.txt (attached to its process). */
typedef struct {
    char *name;      /* heap-allocated resource id, e.g. "x"             */
    int captureTime; /* executed time at which it is captured            */
    int releaseTime; /* executed time at which it is released            */
} ResourceRequest;

/* One line of processes.txt plus the resources it uses. */
typedef struct {
    int id;
    int arrival;
    int runtime;
    int priority;
    int resourceCount;
    int resourceCapacity;
    ResourceRequest *resources;
} ProcessInfo;

/*
 * Appends a resource request to `process` (duplicating `name`).
 * Returns 1 on success, 0 on allocation failure.
 */
int process_add_resource(ProcessInfo *process, const char *name, int captureTime, int releaseTime);

/* Frees all dynamically allocated resources attached to one ProcessInfo. */
void free_process_resources(ProcessInfo *process);

/* Frees an array of ProcessInfo structs and all attached resource data. */
void free_processes(ProcessInfo *list, int count);

/*
 * Reads processes.txt. Allocates *outList (caller frees via free_processes).
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
