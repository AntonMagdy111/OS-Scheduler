/*
 * input.c
 * Parsing of processes.txt and resources.txt (portable C).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "process_info.h"

#define LINE_BUFFER_SIZE 512

/* A line is ignored when it is empty or a comment (starts with '#'). */
static int is_skippable_line(const char *line)
{
    while (*line && isspace((unsigned char)*line))
        line++;
    return *line == '\0' || *line == '#';
}

/* Sort key for processes: arrival time, then ID. */
static int compare_by_arrival(const void *a, const void *b)
{
    const ProcessInfo *first = (const ProcessInfo *)a;
    const ProcessInfo *second = (const ProcessInfo *)b;
    if (first->arrival != second->arrival)
        return first->arrival - second->arrival;
    return first->id - second->id;
}

int load_processes(const char *path, ProcessInfo **outList)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    int capacity = 16;
    int count = 0;
    ProcessInfo *list = malloc(capacity * sizeof(ProcessInfo));
    char line[LINE_BUFFER_SIZE];

    while (list && fgets(line, sizeof line, file)) {
        if (is_skippable_line(line))
            continue;

        ProcessInfo info;
        memset(&info, 0, sizeof info);
        if (sscanf(line, "%d %d %d %d", &info.id, &info.arrival,
                   &info.runtime, &info.priority) != 4) {
            fprintf(stderr, "processes: skipping malformed line: %s", line);
            continue;
        }
        if (info.runtime <= 0) {
            fprintf(stderr, "processes: skipping process %d (runtime <= 0)\n", info.id);
            continue;
        }
        if (count == capacity) {
            capacity *= 2;
            ProcessInfo *grown = realloc(list, capacity * sizeof(ProcessInfo));
            if (!grown) {
                free(list);
                list = NULL;
                break;
            }
            list = grown;
        }
        list[count++] = info;
    }
    fclose(file);

    if (!list)
        return -1;
    /* The file is assumed sorted, but sorting again guarantees ID order
     * for processes arriving at the same second. */
    qsort(list, count, sizeof(ProcessInfo), compare_by_arrival);
    *outList = list;
    return count;
}

static ProcessInfo *find_process(ProcessInfo *list, int count, int id)
{
    for (int i = 0; i < count; i++)
        if (list[i].id == id)
            return &list[i];
    return NULL;
}

void normalize_resource_requests(ProcessInfo *process)
{
    ResourceRequest *requests = process->resources;
    int count = process->resourceCount;

    /* Insertion sort by capture time (at most MAX_RESOURCES_PER_PROCESS items). */
    for (int i = 1; i < count; i++) {
        ResourceRequest key = requests[i];
        int j = i - 1;
        while (j >= 0 && requests[j].captureTime > key.captureTime) {
            requests[j + 1] = requests[j];
            j--;
        }
        requests[j + 1] = key;
    }

    /* Keep only valid, non-overlapping intervals. */
    int kept = 0;
    int previousRelease = 0;
    for (int i = 0; i < count; i++) {
        ResourceRequest *r = &requests[i];
        int valid = r->captureTime >= 1 && r->captureTime < r->releaseTime &&
                    r->releaseTime <= process->runtime &&
                    r->captureTime >= previousRelease;
        if (!valid) {
            fprintf(stderr, "resources: ignoring invalid entry for process %d (%s %d %d)\n",
                    process->id, r->name, r->captureTime, r->releaseTime);
            continue;
        }
        requests[kept++] = *r;
        previousRelease = r->releaseTime;
    }
    process->resourceCount = kept;
}

int load_resources(const char *path, ProcessInfo *list, int count)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    char line[LINE_BUFFER_SIZE];
    char name[64];
    int accepted = 0;

    while (fgets(line, sizeof line, file)) {
        if (is_skippable_line(line))
            continue;

        int pid, capture, release;
        if (sscanf(line, "%d %63s %d %d", &pid, name, &capture, &release) != 4) {
            fprintf(stderr, "resources: skipping malformed line: %s", line);
            continue;
        }
        ProcessInfo *process = find_process(list, count, pid);
        if (!process) {
            fprintf(stderr, "resources: unknown process %d\n", pid);
            continue;
        }
        if (process->resourceCount >= MAX_RESOURCES_PER_PROCESS) {
            fprintf(stderr, "resources: too many entries for process %d\n", pid);
            continue;
        }
        ResourceRequest *request = &process->resources[process->resourceCount++];
        strncpy(request->name, name, MAX_RESOURCE_NAME - 1);
        request->name[MAX_RESOURCE_NAME - 1] = '\0';
        request->captureTime = capture;
        request->releaseTime = release;
        accepted++;
    }
    fclose(file);

    for (int i = 0; i < count; i++)
        normalize_resource_requests(&list[i]);
    return accepted;
}
