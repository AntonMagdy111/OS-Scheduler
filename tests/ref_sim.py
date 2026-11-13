#!/usr/bin/env python3
"""
Independent reference simulator for the OS-scheduler project (test tool only).

It re-implements the rules of the project document with the simplest possible data
structures (plain lists, min() scans, no IPC, no real clock) so that the C scheduler
can be checked against it on many random inputs.

usage: ref_sim.py <processes.txt> <resources.txt> <algo> [quantum] [switch]
       prints scheduler.log lines, then a line "---", then the scheduler.perf lines
"""
import math
import sys


def read_table(path):
    rows = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    rows.append(line.split())
    except FileNotFoundError:
        pass
    return rows


def simulate(procs, uses, algo, quantum=0, cs=1, preempt_at_dispatch=False):
    """procs: [(id, arrival, runtime, priority)], uses: {pid: [(res, capture, release)]}"""
    if algo != 2:
        cs = 1
    P = {}
    for (pid, arr, run, pri) in sorted(procs, key=lambda x: (x[1], x[0])):
        P[pid] = dict(id=pid, arr=arr, run=run, pri=pri, cur=pri, exe=0, state='new', since=0,
                      started=False, held=None, nxt=0,
                      uses=sorted(uses.get(pid, []), key=lambda u: u[1]))
    order = list(P.keys())
    holder, waiters = {}, {}
    ready = []                       # list of pids; RR relies on the order
    log, stats = [], []
    running = None
    target, target_end = None, 0
    slice_left = 0
    done = 0
    t = 0

    def eff(p):
        e = p['cur']
        if algo == 4 and p['state'] == 'ready':
            w = t - p['since']
            bonus = 0 if w < 20 else (w - 20) // 5 + 1
            e = min(e, max(0, p['pri'] - bonus))
        return e

    def line(p, st):
        w = t - p['arr'] - p['exe']
        s = "At time %d process %d %s arr %d total %d remain %d wait %d" % (
            t, p['id'], st, p['arr'], p['run'], p['run'] - p['exe'], w)
        return s

    def best_ready():
        if algo == 1:
            return min(ready, key=lambda i: (P[i]['run'], P[i]['arr'], i))
        if algo == 2:
            return ready[0]
        return min(ready, key=lambda i: (eff(P[i]), i))

    def push_ready(pid):
        P[pid]['state'] = 'ready'
        P[pid]['since'] = t
        ready.append(pid)

    def release(p):
        r, _, _ = p['uses'][p['held']]
        p['held'] = None
        p['cur'] = p['pri']
        holder[r] = None
        ws = waiters.get(r, [])
        if ws:
            w = min(ws, key=lambda i: (P[i]['pri'], i))
            ws.remove(w)
            holder[r] = w
            q = P[w]
            q['held'] = q['nxt']
            q['nxt'] += 1
            q['cur'] = min([q['pri']] + [P[i]['pri'] for i in ws])
            push_ready(w)

    def dispatch(pid):
        nonlocal running, slice_left
        p = P[pid]
        log.append(line(p, 'resumed' if p['started'] else 'started'))
        p['started'] = True
        p['state'] = 'run'
        running = pid
        slice_left = quantum

    def stop_running():
        nonlocal running
        p = P[running]
        log.append(line(p, 'stopped'))
        running = None
        push_ready(p['id'])

    while True:
        t += 1
        if t > 100000:
            raise RuntimeError("simulation does not terminate")
        vacated = False
        just = False

        if target is not None and t >= target_end:
            dispatch(target)
            ready_target = target
            target = None
            just = True

        if running is not None and not just:
            p = P[running]
            p['exe'] += 1
            slice_left -= 1
            if p['held'] is not None and p['uses'][p['held']][2] == p['exe']:
                release(p)
            if p['nxt'] < len(p['uses']) and p['uses'][p['nxt']][1] == p['exe']:
                r = p['uses'][p['nxt']][0]
                if holder.get(r) is None:
                    holder[r] = p['id']
                    p['held'] = p['nxt']
                    p['nxt'] += 1
                else:
                    log.append(line(p, 'stopped'))
                    p['state'] = 'blocked'
                    waiters.setdefault(r, []).append(p['id'])
                    h = P[holder[r]]
                    h['cur'] = min(h['cur'], p['cur'])
                    running = None
                    vacated = True
            if running is not None and p['exe'] == p['run']:
                ta = t - p['arr']
                wta = ta / p['run']
                log.append(line(p, 'finished') + " TA %d WTA %.2f" % (ta, wta))
                stats.append((p['run'], ta - p['run'], wta, t))
                p['state'] = 'done'
                done += 1
                running = None
                vacated = True

        for pid in order:
            if P[pid]['arr'] == t:
                push_ready(pid)

        if algo == 2 and running is not None and slice_left == 0:
            if ready:
                stop_running()
                vacated = True
            else:
                slice_left = quantum

        if algo in (3, 4) and running is not None and ready and (preempt_at_dispatch or not just):
            if eff(P[best_ready()]) < P[running]['cur']:
                stop_running()
                vacated = True

        if running is None and target is None and ready:
            nxt = best_ready()
            ready.remove(nxt)
            if vacated and cs > 0:
                target, target_end = nxt, t + cs
            else:
                dispatch(nxt)

        if done == len(P) and t >= max(p['arr'] for p in P.values()):
            break

    n = len(stats)
    total_run = sum(s[0] for s in stats)
    last = max(s[3] for s in stats)
    wtas = [s[2] for s in stats]
    mean = sum(wtas) / n
    std = math.sqrt(sum((w - mean) ** 2 for w in wtas) / n)
    perf = ["CPU utilization = %d%%" % (total_run * 100 // last),
            "Avg WTA = %.2f" % mean,
            "Avg Waiting = %d" % (sum(s[1] for s in stats) // n),
            "Std WTA = %.2f" % std]
    return log, perf


def main():
    procs_file, res_file, algo = sys.argv[1], sys.argv[2], int(sys.argv[3])
    quantum = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    cs = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    procs = [tuple(int(x) for x in r) for r in read_table(procs_file)]
    uses = {}
    for r in read_table(res_file):
        uses.setdefault(int(r[0]), []).append((r[1], int(r[2]), int(r[3])))
    log, perf = simulate(procs, uses, algo, quantum, cs)
    print("\n".join(log))
    print("---")
    print("\n".join(perf))


if __name__ == '__main__':
    main()
