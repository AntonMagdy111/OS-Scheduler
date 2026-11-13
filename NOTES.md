# OS Scheduler – Phase 1 (CMPS303)

## Build / run
    make                      # builds everything
    ./process_generator.out   # asks for the algorithm (1 SJF, 2 RR, 3 HPF, 4 HPF + demotion bonus)
    ./process_generator.out 2 3 1     # non-interactive: algo [quantum] [switch time]
Input: `processes.txt`, `resources.txt` in the current directory (`./test_generator.out` makes random ones).
Output: `scheduler.log`, `scheduler.perf`.

## Files
| file | role |
|---|---|
| process_generator.c | reads input, asks the user, forks clock + scheduler, sends each process at its arrival second, clears IPC |
| scheduler.c | the scheduler: PCBs, the three algorithms (+ bonus), resources / priority inheritance, log + perf |
| process.c | simulated CPU-bound process; notifies the scheduler when it finishes |
| pcb.h | PCB, Resource, binary heap (ready queue of SJF/HPF), FIFO (ready queue of RR) |
| common.h | algorithm ids, IPC keys, message formats |
| clk.c, headers.h | untouched starter files |

## IPC
* generator -> scheduler: System V message queue. Messages of second T have `mtype = T`; per second: resource usages, the process, then a "second T complete" marker. The scheduler therefore knows it has seen every arrival of second T before simulating it.
* scheduler -> process: `SIGSTOP`/`SIGCONT` + a shared-memory int holding the remaining time.
* process -> scheduler: termination message queue; the scheduler only reaps (never kills) a finished process.
* clock -> everyone: shared memory (`getClk()`).
The scheduler simulates in *logical* seconds, so a few ms of lateness cannot change the log.

## Data structures
SJF: min-heap on (runtime, arrival, id). HPF: min-heap on (effective priority, id), O(log n) re-key when a holder inherits a priority. HPF + demotion: same heap, rebuilt (O(n)) every second because aging changes all keys. RR: linked FIFO. Resources: holder + array of waiters.

## Per-second event order (as in the document)
0. a context switch that ends now completes (the process starts/resumes) 1. running process: release -> capture (or block) -> finish 2. arrivals by id 3. RR quantum expiry, HPF preemption check, choice of next process. A switch costs time only when the CPU moves directly from one process to another; an idle CPU starts a process immediately.

## Assumption to confirm with your TA
The document does not say what happens when a **higher-priority process arrives in the very second a context switch ends** (HPF family). Default here: the process that just started runs at least one second, and the newcomer preempts one second later. The other reading (preempt at once, logged `started` and `stopped` at the same second) is a one-line switch: compile `scheduler.c` with `-DPREEMPT_AT_DISPATCH=1`. About 45% of dense random HPF inputs differ between the two readings, so it is worth asking. A choice that lets such a newcomer win the CPU at the end of the switch (decide at switch end instead of switch start) is a third possible reading; it would also affect SJF.

Smaller choices: RR accepts a context-switch time of 0; malformed input files are rejected with a message instead of being guessed at.

## Tests (`tests/`)
* `cases/*` – the five examples of the document with their expected logs/perf; `run_case.sh <case> [tick_us]` runs one (1000000 = real clock).
* `ref_sim.py` – independent plain-Python reference simulator; `random_compare.py N tick_us seed` compares it with the C scheduler on random inputs (needs `gcc -std=gnu11 tests/fast_clk.c -o tests/fast_clk.out` first: same as clk.c but with a shorter second).
