/*
 * input.c
 * Parsing of processes.txt and resources.txt (portable C).
 * Uses dynamic line reading and dynamic resource arrays so arbitrary resource
 * name lengths and arbitrary numbers of resource entries per process work.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "process_info.h"

/* Reads an arbitrarily long line from `file`. Caller frees the returned buffer. */
static char *read_line_dynamic(FILE *file)
{
    size_t capacity = 256;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;

    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }
        buffer[length++] = (char)ch;
        if (ch == '\n')
            break;
    }
    if (length == 0 && ch == EOF) {
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
}

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

static char *duplicate_string(const char *text)
{
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy)
        memcpy(copy, text, len + 1);
    return copy;
}

int process_add_resource(ProcessInfo *process, const char *name, int captureTime, int releaseTime)
{
    if (process->resourceCount == process->resourceCapacity) {
        int newCapacity = process->resourceCapacity ? process->resourceCapacity * 2 : 4;
        ResourceRequest *grown = realloc(process->resources,
                                         (size_t)newCapacity * sizeof(ResourceRequest));
        if (!grown)
            return 0;
        process->resources = grown;
        process->resourceCapacity = newCapacity;
    }
    char *nameCopy = duplicate_string(name);
    if (!nameCopy)
        return 0;
    ResourceRequest *request = &process->resources[process->resourceCount++];
    request->name = nameCopy;
    request->captureTime = captureTime;
    request->releaseTime = releaseTime;
    return 1;
}

void free_process_resources(ProcessInfo *process)
{
    if (!process)
        return;
    for (int i = 0; i < process->resourceCount; i++)
        free(process->resources[i].name);
    free(process->resources);
    process->resources = NULL;
    process->resourceCount = 0;
    process->resourceCapacity = 0;
}

void free_processes(ProcessInfo *list, int count)
{
    if (!list)
        return;
    for (int i = 0; i < count; i++)
        free_process_resources(&list[i]);
    free(list);
}

int load_processes(const char *path, ProcessInfo **outList)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    int capacity = 16;
    int count = 0;
    ProcessInfo *list = malloc((size_t)capacity * sizeof(ProcessInfo));
    if (!list) {
        fclose(file);
        return -1;
    }

    char *line;
    while ((line = read_line_dynamic(file)) != NULL) {
        if (is_skippable_line(line)) {
            free(line);
            continue;
        }

        ProcessInfo info;
        memset(&info, 0, sizeof info);
        if (sscanf(line, "%d %d %d %d", &info.id, &info.arrival,
                   &info.runtime, &info.priority) != 4) {
            fprintf(stderr, "processes: skipping malformed line: %s", line);
            free(line);
            continue;
        }
        free(line);

        if (info.runtime <= 0) {
            fprintf(stderr, "processes: skipping process %d (runtime <= 0)\n", info.id);
            continue;
        }
        if (count == capacity) {
            capacity *= 2;
            ProcessInfo *grown = realloc(list, (size_t)capacity * sizeof(ProcessInfo));
            if (!grown) {
                free_processes(list, count);
                fclose(file);
                return -1;
            }
            list = grown;
        }
        list[count++] = info;
    }
    fclose(file);

    /* The file is assumed sorted, but sorting again guarantees ID order
     * for processes arriving at the same second. */
    qsort(list, (size_t)count, sizeof(ProcessInfo), compare_by_arrival);
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

    /* Insertion sort by capture time. */
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
        int valid = r->name != NULL &&
                    r->captureTime >= 1 && r->captureTime < r->releaseTime &&
                    r->releaseTime <= process->runtime &&
                    r->captureTime >= previousRelease;
        if (!valid) {
            fprintf(stderr, "resources: ignoring invalid entry for process %d (%s %d %d)\n",
                    process->id, r->name ? r->name : "", r->captureTime, r->releaseTime);
            free(r->name);
            r->name = NULL;
            continue;
        }
        requests[kept++] = *r;
        previousRelease = r->releaseTime;
    }
    process->resourceCount = kept;
}

/* Parses `<pid> <resource_name> <capture> <release>` with arbitrary name length. */
static int parse_resource_line(const char *line, int *pid, char **nameOut, int *capture, int *release)
{
    const char *p = line;
    char *end;

    long pidVal = strtol(p, &end, 10);
    if (end == p)
        return 0;
    p = end;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return 0;

    const char *nameStart = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    size_t nameLen = (size_t)(p - nameStart);
    if (nameLen == 0)
        return 0;

    long capVal = strtol(p, &end, 10);
    if (end == p)
        return 0;
    p = end;

    long relVal = strtol(p, &end, 10);
    if (end == p)
        return 0;

    char *name = malloc(nameLen + 1);
    if (!name)
        return 0;
    memcpy(name, nameStart, nameLen);
    name[nameLen] = '\0';

    *pid = (int)pidVal;
    *nameOut = name;
    *capture = (int)capVal;
    *release = (int)relVal;
    return 1;
}

int load_resources(const char *path, ProcessInfo *list, int count)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    char *line;
    int accepted = 0;

    while ((line = read_line_dynamic(file)) != NULL) {
        if (is_skippable_line(line)) {
            free(line);
            continue;
        }

        int pid, capture, release;
        char *name = NULL;
        if (!parse_resource_line(line, &pid, &name, &capture, &release)) {
            fprintf(stderr, "resources: skipping malformed line: %s", line);
            free(line);
            continue;
        }
        free(line);

        ProcessInfo *process = find_process(list, count, pid);
        if (!process) {
            fprintf(stderr, "resources: unknown process %d\n", pid);
            free(name);
            continue;
        }
        if (!process_add_resource(process, name, capture, release)) {
            fprintf(stderr, "resources: out of memory adding entry for process %d\n", pid);
            free(name);
            fclose(file);
            return -1;
        }
        free(name);
        accepted++;
    }
    fclose(file);

    for (int i = 0; i < count; i++)
        normalize_resource_requests(&list[i]);
    return accepted;
}
