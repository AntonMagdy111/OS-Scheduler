# OS Scheduler — CMPS303 Project Report

## 1. Data Structures Used

- **Process Control Block (`PCB` in `pcb.h`)**: Stores static metadata (`id`, `arrivalTime`, `runtime`, `originalPriority`), dynamic execution state (`state`, `executedTime`, `hasStarted`, `readySince`, `priorityKey`, `inheritedPriority`), resource usage tracking (`resourceUses`, `heldResource`, `waitingFor`), intrusive container links (`heapIndex`, `nextInQueue`, `nextWaiter`, `prevLive`, `nextLive`), and OS IPC handles (`systemPid`, `sharedMemoryId`, `sharedRemainingTime`).
- **Indexed Binary Min-Heap (`MinHeap` in `data_structures.h` / `data_structures.c`)**: Used as the ready structure for SJF (keyed by `(runtime, arrivalTime, id)`) and HPF / HPF-Aging (keyed by `(priorityKey, id)`). Storing `heapIndex` inside each `PCB` enables $O(\log n)$ in-place re-heapification (`heap_update`) when priority inheritance changes a ready holder's key, and $O(n)$ `heap_rebuild` on aging ticks.
- **Intrusive FIFO Queue (`FifoQueue` in `data_structures.h` / `data_structures.c`)**: Singly-linked queue with `head` and `tail` pointers providing $O(1)$ enqueue and dequeue for Round Robin.
- **Resource Hash Table (`ResourceTable` in `resource.h` / `resource.c`)**: Chained hash table (64 buckets, djb2 string hash) mapping dynamically allocated resource names (arbitrary length) to `Resource` structs. Each `Resource` keeps a pointer to its current `holder` and a `waiters` list sorted by `(originalPriority, id)`, giving $O(1)$ retrieval of the highest-priority waiter on release (`resource_pop_waiter`) and $O(1)$ lookup of the highest waiter priority for inheritance (`resource_best_waiter_priority`).
- **Dynamic Process & Resource Parsing (`process_info.h` / `input.c`)**: Dynamically sized process and per-process resource arrays with dynamic line reading and chunked System V IPC streaming (`MSG_RESOURCE` + `MSG_ARRIVAL` + `MSG_TICK_END`), supporting arbitrary numbers of resource requests per process and arbitrary resource name lengths without truncation.

## 2. Algorithm Explanations and Results

Every clock tick `now` processes events in the specified order:
1. Running process events: increment `executedTime`, process resource release, then resource capture (blocking if held), then wait for worker termination notice (`onFinish`) and finish if `executedTime == runtime`.
2. Arrivals at `now` sorted by `id`.
3. Context-switch completion (`now >= switchEnd`), aged priority refresh (Algorithm 4), RR quantum expiry / HPF preemption check, and next-process selection (`choose_next`).

### Implemented Algorithms
1. **Shortest Job First (SJF, Algorithm 1)**: Non-preemptive; selects the ready process with minimum `(runtime, arrivalTime, id)` from the min-heap. Context switch = 1 sec.
2. **Round Robin (RR, Algorithm 2)**: Preemptive with user-supplied `quantum` and `contextSwitchTime` (default 1). Arrivals at a quantum-expiry tick are enqueued before the preempted process. If the ready queue is empty at quantum expiry, the running process continues with no stop, switch, or log line.
3. **Preemptive HPF with Priority Inheritance (Algorithm 3)**: Priorities range from `0` (highest) to `10` (lowest). A ready process preempts the running process only if its effective priority is strictly smaller (higher priority). Blocking on an occupied resource transfers the waiter's priority to the holder if higher (`min(originalPriority, inheritedPriority)`). On resource release, inherited priority is dropped and the resource transfers to the highest-priority waiter (tie-broken by smaller `id`).
4. **Bonus — HPF with Priority Demotion / Aging (Algorithm 4)**: Tracks consecutive seconds $w = \text{now} - \text{readySince}$ spent in the Ready state (including context-switch time; Blocked processes do not age). For $w \ge 20$, $\text{bonus} = \lfloor (w - 20)/5 \rfloor + 1$ and $\text{effective priority} = \max(0, \text{originalPriority} - \text{bonus})$. Dispatching resets $w$ to 0 (demotion).

### Verified Test Results (46 / 46 Passing)

Core specification and targeted edge-case suites (in addition to 34 extended stress & edge-case suites in `tests/`, all 46 passing `make test` with exact log-header, trace, and performance verification):

| Test Case | Algorithm | CPU Utilization | Avg WTA | Avg Waiting | Std WTA |
|---|---|---:|---:|---:|---:|
| `sjf_example` | SJF (1) | 78% | 3.06 | 6 | 1.20 |
| `sjf_ties_arrival_and_id` | SJF (1) | 76% | 3.73 | 7 | 1.72 |
| `rr_example` | RR (2, q=3, cs=1) | 68% | 5.59 | 10 | 3.30 |
| `rr_quantum_continuation` | RR (2, q=3, cs=1) | 58% | 1.00 | 0 | 0.00 |
| `rr_arrival_and_switch_order` | RR (2, q=2, cs=2) | 53% | 3.33 | 5 | 1.55 |
| `hpf_output_example` | HPF (3, cs=1) | 75% | 1.58 | 3 | 0.25 |
| `hpf_inheritance_example` | HPF (3, cs=1) | 65% | 3.31 | 10 | 0.32 |
| `hpf_multi_waiters_and_chained_resources` | HPF (3, cs=1) | 63% | 3.78 | 10 | 0.68 |
| `hpf_many_and_long_resources` | HPF (3, cs=1) | 81% | 3.05 | 9 | 1.30 |
| `hpf_plain_on_aging_input` | HPF (3, cs=1) | 90% | 3.11 | 13 | 2.63 |
| `hpf_aging_example` | HPF-Aging (4, cs=1) | 88% | 3.20 | 15 | 2.69 |
| `hpf_aging_switch_tie` | HPF-Aging (4, cs=1) | 88% | 6.01 | 22 | 3.68 |

### Comparison: Plain HPF (Algorithm 3) vs. HPF with Priority Demotion (Algorithm 4)

On the aging workload (`P1(arr=1, run=30, prio=5)`, `P2(arr=2, run=6, prio=5)`, `P3(arr=25, run=3, prio=4)`):
- **Starvation & Preemptions**: In Plain HPF (`hpf_plain_on_aging_input`), P2 waits `35` seconds behind P1 and P3 and never preempts P1 (`1` preemption total: P3 preempting P1 at `t=25`). With Priority Demotion (`hpf_aging_example`), P2's consecutive Ready time reaches `w=21` at `t=23` (`bonus=1`, effective priority `4 < 5`), so P2 preempts P1 at `t=22` and starts at `t=23` without starving indefinitely if higher-priority arrivals continued.
- **Overhead & Metrics**: The extra preemption caused by aging adds a second context switch (CPU utilization drops from `90%` to `88%`), and demoting P2 back to priority `5` upon dispatch allows P3 (`prio 4`) to preempt P2 at `t=25`, increasing P1's wait from `5` to `8` and Avg Waiting from `13` to `15` (Avg WTA `3.11` -> `3.20`, Std WTA `2.63` -> `2.69`). Thus, aging bounds starvation at the cost of additional preemptions and context-switch overhead.

## 3. Assumptions

Only assumptions specified in the project document are used:
- Simulation clock starts at time `1`; earliest arrival is `1`.
- All time values are integers (1-second granularity); context switch is fixed at `1` second for SJF and HPF, and user-provided (default `1`) for RR.
- First process on an idle CPU starts with 0 context-switch overhead.
- Processes arriving at the same second are ordered by `id`.
- Priority values are in `[0, 10]` (`0` highest, `10` lowest); equal-priority processes do not preempt the running process and tie-break by smaller `id`.
- Each resource entry satisfies `1 <= capture < release <= runtime`, intervals for the same process do not overlap, and a process holds at most one resource at a time.
- CPU utilization and Avg Waiting are floored to integers; WTA, Avg WTA, and Std WTA (population standard deviation) are rounded to 2 decimal places.
- **Arrival / Aging at Context-Switch End (HPF family)**: When a context switch completes at second $t$, the target process chosen when the switch began is dispatched at $t$ and runs for at least 1 second before any newcomer arriving at $t$ can preempt it (avoiding `started` and `stopped` log lines at the identical timestamp $t$). If the TA prefers immediate 0-second preemption at $t$ or choosing the target at the end of the switch, `sched_core.c` supports `-DPREEMPT_AT_DISPATCH=1` and `-DCHOOSE_AT_SWITCH_END=1` at compile time.

## 4. Workload Distribution & Time Log

| Task | Owner | Time Taken |
|---|---|---:|
| Scheduling engine, algorithms & PCB (`sched_core.c`, `sched_core.h`, `pcb.h`) | Anton Magdy | 4.0 h |
| Linux IPC, clock synchronization & process lifecycle (`process_generator.c`, `scheduler.c`, `process.c`, `headers.h`) | Anton Magdy & Kiro Ashraf | 3.5 h |
| Input parsing & data structures (`input.c`, `data_structures.c`, `resource.c`, `process_info.h`) | Kiro Ashraf | 2.5 h |
| GCC 15 / GNU C17 build fix, Linux WSL2 build & code review (`Makefile`, IPC error-handling audit) | Kerolos Nasser | 2.0 h |
| Hard edge-case & stress test suite, verification & report (`tests/`, `sim.c`, `REPORT.md`) | Ahmed Wael Sadek El Sawy & Anton Magdy | 2.5 h |
