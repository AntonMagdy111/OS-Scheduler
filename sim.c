/*
 * sim.c
 * Offline driver for the scheduling engine (no IPC, no real clock).
 * It feeds the engine one second at a time and produces the same
 * scheduler.log / scheduler.perf as the full system, instantly.
 * Used for regression tests (tests/run_tests.sh) and for the report.
 *
 * Usage: sim.out <processes.txt> <resources.txt> <algorithm 1-4>
 *                [quantum] [context_switch] [log_path] [perf_path]
 */
#include <stdio.h>
#include <stdlib.h>

#include "process_info.h"
#include "sched_core.h"

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <processes> <resources> <algorithm 1-4> "
                        "[quantum] [context_switch] [log] [perf]\n", argv[0]);
        return 1;
    }
    const char *processesPath = argv[1];
    const char *resourcesPath = argv[2];
    Algorithm algorithm = (Algorithm)atoi(argv[3]);
    int quantum = argc > 4 ? atoi(argv[4]) : 1;
    int contextSwitch = (algorithm == ALGO_RR && argc > 5) ? atoi(argv[5]) : 1;
    const char *logPath = argc > 6 ? argv[6] : "scheduler.log";
    const char *perfPath = argc > 7 ? argv[7] : "scheduler.perf";

    if (algorithm < ALGO_SJF || algorithm > ALGO_HPF_AGING || quantum <= 0 || contextSwitch < 0) {
        fprintf(stderr, "Invalid algorithm/quantum/context switch\n");
        return 1;
    }

    ProcessInfo *processes = NULL;
    int count = load_processes(processesPath, &processes);
    if (count < 0) {
        fprintf(stderr, "Cannot open %s\n", processesPath);
        return 1;
    }
    if (load_resources(resourcesPath, processes, count) < 0)
        fprintf(stderr, "Note: %s not found, running without resources\n", resourcesPath);

    FILE *logFile = fopen(logPath, "w");
    FILE *perfFile = fopen(perfPath, "w");
    if (!logFile || !perfFile) {
        fprintf(stderr, "Cannot create output files\n");
        if (logFile) fclose(logFile);
        if (perfFile) fclose(perfFile);
        free_processes(processes, count);
        return 1;
    }

    Scheduler scheduler;
    if (!scheduler_init(&scheduler, algorithm, quantum, contextSwitch, logFile, NULL)) {
        fprintf(stderr, "Out of memory\n");
        fclose(logFile);
        fclose(perfFile);
        free_processes(processes, count);
        return 1;
    }

    int nextProcess = 0;
    for (int now = 1; nextProcess < count || !scheduler_is_idle(&scheduler); now++) {
        int first = nextProcess;
        while (nextProcess < count && processes[nextProcess].arrival <= now)
            nextProcess++;
        scheduler_tick(&scheduler, now, &processes[first], nextProcess - first);
    }

    scheduler_write_perf(&scheduler, perfFile);
    scheduler_destroy(&scheduler);
    fclose(logFile);
    fclose(perfFile);
    free_processes(processes, count);
    return 0;
}
