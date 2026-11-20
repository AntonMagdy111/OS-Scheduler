# OS Scheduler — Project Report
**Cairo University — Faculty of Engineering — Credit Hour System**  
**Course:** Operating Systems (CMPS303)

---

## 1. Data Structures Used

| Data Structure | File(s) | Purpose | Complexity |
|---|---|---|---|
| **Process Control Block (`PCB`)** | `pcb.h` | Stores static metadata (`id`, `arrivalTime`, `runtime`, `originalPriority`), dynamic state (`state`, `executedTime`, `hasStarted`, `readySince`, `priorityKey`, `inheritedPriority`), resource usage list & locks (`resourceUses`, `heldResource`, `waitingFor`), intrusive links (`heapIndex`, `nextInQueue`, `nextWaiter`, `prevLive`, `nextLive`), and OS IPC handles (`systemPid`, `sharedMemoryId`, `sharedRemainingTime`). | $O(1)$ field access |
| **Indexed Binary Min-Heap (`MinHeap`)** | `data_structures.h`, `data_structures.c` | Ready queue for **SJF** (ordered by `(runtime, arrivalTime, id)`) and **HPF / HPF-Aging** (ordered by `(priorityKey, id)`). Storing `heapIndex` inside each `PCB` allows in-place key updates (`heap_update`) when priority inheritance boosts a Ready resource holder, and linear-time `heap_rebuild` on aging ticks. | Push/Pop/Update: $O(\log n)$ · Peek: $O(1)$ · Rebuild: $O(n)$ |
| **Intrusive FIFO Queue (`FifoQueue`)** | `data_structures.h`, `data_structures.c` | Singly-linked queue (`head`, `tail`) used as the Ready queue for **Round Robin**. | Enqueue / Dequeue / Peek: $O(1)$ |
| **Chained Hash Table & Sorted Waiter List (`ResourceTable`, `Resource`)** | `resource.h`, `resource.c` | 64-bucket chained hash table (djb2 hash) mapping dynamically allocated resource names (arbitrary length) to `Resource` structs. Each `Resource` stores its current `holder` and a `waiters` list sorted by `(originalPriority, id)`. | Lookup: $O(1)$ avg · Best waiter / Transfer: $O(1)$ · Insert waiter: $O(w)$ |
| **Dynamic Process & Resource Arrays (`ProcessInfo`, `ResourceRequest`)** | `process_info.h`, `input.c` | Dynamically resized arrays with dynamic line reading for `processes.txt` and `resources.txt` (no fixed cap on resource count per process or resource name length). | Amortized $O(1)$ append |
| **Live PCB Doubly-Linked List** | `pcb.h`, `sched_core.c` | Tracks all active processes (`liveHead`, `liveCount`) for $O(1)$ insertion/deletion and clean emergency teardown on `SIGINT`/`SIGTERM`. | Insert / Delete: $O(1)$ |
| **Dynamic Arrival & Statistics Buffers (`ArrivalBuffer`, `wtaValues`)** | `scheduler.c`, `sched_core.h` | Growable arrays storing per-tick arrivals received over IPC and exact unrounded per-process `WTA` doubles for computing `Avg WTA` and population `Std WTA`. | Amortized $O(1)$ append |

### IPC Mechanisms (`headers.h`, `process_generator.c`, `scheduler.c`, `process.c`, `clk.c`)
- **Clock Shared Memory (`SHKEY = 300`)**: Emulated integer clock incremented every 1 second by `clk.out` and read via `getClk()`.
- **Generator → Scheduler Message Queue (`GENERATOR_MSGQ_KEY = 400`)**: Streams each arriving process's resource requests (`MSG_RESOURCE`, chunked for arbitrarily long names) followed by `MSG_ARRIVAL`, then a `MSG_TICK_END` barrier per clock second so the scheduler never misses or reorders arrivals.
- **Scheduler → Process Control**: `fork()` + `execl("./process.out")` on first dispatch, `SIGSTOP` / `SIGCONT` on preemption and resumption, plus a private shared-memory integer (`IPC_PRIVATE`) publishing each process's remaining time.
- **Process → Scheduler Termination Queue (`PROCESS_MSGQ_KEY = 500`)**: When `remainingtime` reaches `0`, the worker sends its completion message (`mtype = id + 1`) and terminates on its own. The scheduler polls `msgrcv(IPC_NOWAIT)` alongside `waitpid(WNOHANG)` so a worker crash never hangs the simulation, then reaps it and deletes its PCB and shared memory.

---

## 2. Algorithm Explanations and Results

### Per-Second Event Order
At every integer clock second `now`, `scheduler_tick()` (`sched_core.c`) executes in the exact order specified by the project document:
1. **Running process events**: increment `executedTime` and update shared memory; if `executedTime == heldReleaseTime`, release the held resource (transferring it directly to the highest-priority blocked waiter); next, if `executedTime == next captureTime`, capture the resource if free or block the process (boosting the holder via priority inheritance); finally, if `executedTime == runtime`, await the worker's termination notice, log `finished`, update stats, and delete the PCB.
2. **Arrivals at `now`**: inserted into the Ready structure in ascending `id` order.
3. **Switch completion, aging, preemption, and next-process choice**:
   - Complete any context switch ending at `now` (`dispatch` target).
   - Refresh aged priorities (Algorithm 4 only).
   - Check RR quantum expiry (continue without logging if Ready queue is empty; otherwise preempt and enqueue after arrivals) or HPF preemption (`ready_peek->priorityKey < running_priority`).
   - If the CPU is free and Ready is non-empty: start immediately with `0` overhead if the CPU was idle, or begin a context switch of length `contextSwitchTime` if a process just left the CPU at `now`.

### Implemented Scheduling Algorithms
1. **Shortest Job First (SJF — Algorithm 1)**: Non-preemptive; picks the Ready process with smallest `(runtime, arrivalTime, id)`. Context switch is fixed at `1` sec.
2. **Round Robin (RR — Algorithm 2)**: Preemptive FIFO rotation with user-supplied `quantum` and `contextSwitchTime` (default `1` sec). Arrivals at quantum expiry enter the queue ahead of the preempted process; arrivals during a context switch enter behind the already-queued preempted process.
3. **Preemptive HPF with Priority Inheritance (Algorithm 3)**: Priorities range from `0` (highest) to `10` (lowest); strictly higher priority preempts, equal priorities do not preempt and tie-break by smaller `id`. Blocking on a held resource grants the holder `min(originalPriority, bestWaiterPriority)`. On release, inheritance is dropped and the resource transfers to the highest-priority waiter (smaller `id` on ties). Context switch is fixed at `1` sec.
4. **Bonus — HPF with Priority Demotion / Aging (Algorithm 4)**: Extends Algorithm 3 with consecutive Ready waiting counter $w = \text{now} - \text{readySince}$ (includes context-switch time; Blocked processes do not age). For $w < 20$, $\text{bonus} = 0$; for $w \ge 20$, $\text{bonus} = \lfloor(w - 20)/5\rfloor + 1$ and $\text{effective priority} = \max(0, \text{originalPriority} - \text{bonus})$. A Ready holder uses $\min(\text{effective priority}, \text{inheritedPriority})$. Upon dispatch (`started` or `resumed`), $w$ resets to `0` and the process is demoted back to `originalPriority`.

### Verified Test Results (46 / 46 Passing)
All 5 official PDF examples reproduce character-for-character on both the offline driver (`sim.out`) and the live Linux IPC system (`process_generator.out` + `clk.out` + `scheduler.out` + `process.out`), along with 41 additional edge-case and stress tests in `tests/`:

| Test Case | Algorithm | CPU Utilization | Avg WTA | Avg Waiting | Std WTA |
|---|---|---:|---:|---:|---:|
| `sjf_example` *(PDF)* | SJF (1) | 78% | 3.06 | 6 | 1.20 |
| `sjf_ties_arrival_and_id` | SJF (1) | 76% | 3.73 | 7 | 1.72 |
| `rr_example` *(PDF)* | RR (2, q=3, cs=1) | 68% | 5.59 | 10 | 3.30 |
| `rr_quantum_continuation` | RR (2, q=3, cs=1) | 58% | 1.00 | 0 | 0.00 |
| `rr_arrival_and_switch_order` | RR (2, q=2, cs=2) | 53% | 3.33 | 5 | 1.55 |
| `hpf_output_example` *(PDF)* | HPF (3, cs=1) | 75% | 1.58 | 3 | 0.25 |
| `hpf_inheritance_example` *(PDF)* | HPF (3, cs=1) | 65% | 3.31 | 10 | 0.32 |
| `hpf_multi_waiters_and_chained_resources` | HPF (3, cs=1) | 63% | 3.78 | 10 | 0.68 |
| `hpf_many_and_long_resources` | HPF (3, cs=1) | 81% | 3.05 | 9 | 1.30 |
| `hpf_plain_on_aging_input` | HPF (3, cs=1) | 90% | 3.11 | 13 | 2.63 |
| `hpf_aging_example` *(PDF Bonus)* | HPF-Aging (4, cs=1) | 88% | 3.20 | 15 | 2.69 |
| `hpf_aging_switch_tie` | HPF-Aging (4, cs=1) | 88% | 6.01 | 22 | 3.68 |

### Bonus Evaluation: Plain HPF (Algorithm 3) vs. HPF with Priority Demotion (Algorithm 4)

Measured results running identical workloads on Algorithm 3 vs. Algorithm 4:

| Workload | Algo | CPU Util | Avg WTA | Avg Wait | Std WTA | Stops (Preempt/Block) | Avg First-Start Wait |
|---|---|---:|---:|---:|---:|---:|---:|
| **PDF Aging Example** (3 procs) | Algo 3 (Plain HPF) | 90% | 3.11 | 13 | 2.63 | 1 | 12.0 s |
| | Algo 4 (Aging) | 88% | 3.20 | 15 | 2.69 | 2 | 7.3 s |
| **Multi-Waiter Inheritance** (5 procs) | Algo 3 (Plain HPF) | 73% | 5.35 | 23 | 1.40 | 6 | 9.8 s |
| | Algo 4 (Aging) | 69% | 5.60 | 24 | 1.59 | 8 | 9.6 s |
| **Many Holders** (9 procs, 4 res) | Algo 3 (Plain HPF) | 85% | 4.91 | 32 | 1.96 | 3 | 27.7 s |
| | Algo 4 (Aging) | 79% | 5.38 | 36 | 2.27 | 9 | 23.9 s |
| **Long Jobs Stress** (30 procs, up to 60s) | Algo 3 (Plain HPF) | 96% | 26.02 | 472 | 39.19 | 5 | 433.5 s |
| | Algo 4 (Aging) | 68% | 39.99 | 942 | 30.27 | 468 | 253.2 s |
| **Dense Contention** (80 procs, 5 res) | Algo 3 (Plain HPF) | 83% | 78.41 | 232 | 112.03 | 7 | 230.3 s |
| | Algo 4 (Aging) | 55% | 89.80 | 350 | 71.54 | 237 | 184.9 s |

**Conclusions from Measured Results:**
- **Eliminates Starvation & Cuts First-Response Delay**: Under heavy load (`Long Jobs Stress`), aging cuts average time-to-first-start by **42%** (`433.5 s → 253.2 s`, max wait-to-start `955 s → 587 s`) and improves turnaround fairness significantly (`Std WTA` drops from `39.19 → 30.27` on 30 long jobs and `112.03 → 71.54` on 80 dense jobs — a **36%** reduction in WTA spread).
- **Cost of Demotion (Preemption & Context-Switch Overhead)**: Because demotion resets a promoted process back to its low original priority immediately upon dispatch, other aged Ready processes quickly preempt it after 1 second. On the 30-job and 80-job stress suites, `stopped` events jump from `5 → 468` and `7 → 237`. With a 1-second context-switch penalty per preemption, CPU utilization drops (`96% → 68%`, `83% → 55%`) and average completion wait / Avg WTA rise (`472 → 942` and `232 → 350`). Thus, aging with immediate demotion guarantees progress for starving processes at the cost of throughput under dense long-running workloads.

---

## 3. Assumptions

### A. Specification Assumptions (From Project Document)
- The simulation clock starts at time `1` (earliest possible process arrival is `1`), and all time quantities are integers (`1` sec granularity).
- Context-switch overhead is fixed at `1` second for SJF, HPF, and HPF-Aging, and user-provided (default `1` sec) for Round Robin. An idle CPU dispatches with `0` overhead.
- Processes arriving at the same second are ordered by ascending `id`.
- Priority values are in `[0, 10]` (`0` highest, `10` lowest); equal-priority processes do not preempt the running process and tie-break by smaller `id`.
- Resource entries obey `1 <= capture < release <= runtime`, intervals for the same process do not overlap, and a process holds at most one resource at a time (preventing circular-wait deadlocks).
- `CPU utilization` and `Avg Waiting` are floored to integers; `WTA`, `Avg WTA`, and population `Std WTA` are rounded to 2 decimal places.

### B. Implementation Assumptions (For TA Confirmation)
1. **Higher-Priority Arrival or Aging at the Exact Second a Context Switch Ends (HPF / HPF-Aging)**: The specification does not explicitly specify whether a process arriving or aging at the exact second `t = switchEnd` should preempt the newly dispatched target at `t` with `0` seconds of execution. By default, our scheduler chooses the switch target when the switch begins, dispatches it at `switchEnd`, and lets it execute for at least `1` second before preemption (avoiding `started` and `stopped` log lines at the identical timestamp `t`). Because the PDF requires TA permission for extra assumptions, `sched_core.c` also supports compile-time flags `-DPREEMPT_AT_DISPATCH=1` (immediate 0-second preemption) and `-DCHOOSE_AT_SWITCH_END=1` (defer target choice to `switchEnd`) so the behavior can be switched in one flag if preferred.
2. **Cooperative `1 ms` Loop Sleep in `process.c`**: Because the scheduler tracks each process's CPU execution time via shared memory and `SIGSTOP`/`SIGCONT`, `process.c` sleeps `1 ms` (`usleep(1000)`) per loop iteration so it does not starve `clk.out` or `scheduler.out` on a single-core VM or WSL environment.
3. **Round Robin Context-Switch Time of `0`**: Accepted as a valid non-negative integer input (`0` overhead between quanta).
4. **Defensive Input & Worker Error Handling**: Malformed lines or invalid/overlapping resource entries are ignored with a warning to `stderr`; worker crash or `fork()` failure aborts cleanly with non-zero exit status and full IPC cleanup rather than hanging or producing partial results.

---

## 4. Workload Distribution

| Team Member | Responsibilities |
|---|---|
| **Anton Magdy** | Scheduling engine & all 4 algorithms (`sched_core.c`, `sched_core.h`, `pcb.h`), real-time IPC coordination, Bonus evaluation, and report synthesis. |
| **Kirolos Ashraf** | Core data structures (`data_structures.c`, `resource.c`), dynamic input parsing (`input.c`, `process_info.h`), worker process (`process.c`), edge-case & stress test suite (`tests/`), and `scheduler.log` / `scheduler.perf` verification. |
| **Kerolos Nasser** | Linux WSL2 / GCC 15 GNU C17 build compatibility (`Makefile`), full code & IPC error-handling review (`scheduler.c`, `process_generator.c`), and live Linux testing. |

---

## 5. Time Taken for Each Task

| Task | Owner(s) | Time Taken |
|---|---|---:|
| System design, PCB & scheduling engine (`sched_core.c`, `pcb.h`) | Anton Magdy | 12.0 h |
| Data structures & dynamic input parsing (`data_structures.c`, `resource.c`, `input.c`) | Kirolos Ashraf | 7.0 h |
| Linux System V IPC, clock sync & worker lifecycle (`process_generator.c`, `scheduler.c`, `process.c`) | Anton Magdy, Kirolos Ashraf | 10.0 h |
| GCC 15 / C17 build fix, IPC failure-path code review & live Linux testing | Kerolos Nasser | 6.0 h |
| Test suite creation (46 cases), stress testing & Bonus A3 vs. A4 evaluation | Kirolos Ashraf, Anton Magdy | 8.0 h |
| Report writing & documentation (`REPORT.md`, `REPORT.pdf`, `README`) | Anton Magdy, Kirolos Ashraf | 4.0 h |
| **Total** | | **47.0 h** |
