#!/usr/bin/env python3
"""Random differential test: C scheduler (fast clock) vs ref_sim.py.
usage: random_compare.py <num cases> [tick_us] [seed]"""
import os, random, subprocess, sys, shutil
sys.path.insert(0, os.path.dirname(__file__))
from ref_sim import simulate

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
N = int(sys.argv[1]); TICK = sys.argv[2] if len(sys.argv) > 2 else "30000"
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rnd = random.Random(seed)

def gen_case():
    n = rnd.randint(1, 9)
    style = rnd.choice(["dense", "sparse", "mixed"])
    maxrt = rnd.choice([3, 6, 12])
    prio_range = rnd.choice([2, 4, 11])
    nres = rnd.choice([0, 1, 2, 3])
    procs, uses = [], {}
    arr = 1
    for i in range(1, n + 1):
        if i > 1:
            arr += rnd.choice([0, 0, 1, 2]) if style == "dense" else rnd.choice([0, 1, 3, 8]) if style == "mixed" else rnd.randint(0, 12)
        run = rnd.randint(1, maxrt)
        pri = rnd.randint(0, prio_range - 1)
        procs.append((i, arr, run, pri))
        cursor, lst = 1, []
        for _ in range(rnd.randint(0, 3) if nres else 0):
            if cursor > run - 1: break
            cap = rnd.randint(cursor, run - 1)
            rel = rnd.randint(cap + 1, run)
            lst.append(("r%d" % rnd.randint(1, nres), cap, rel))
            cursor = rel
        if lst: uses[i] = lst
    return procs, uses

def write_case(d, procs, uses):
    with open(os.path.join(d, "processes.txt"), "w") as f:
        f.write("#id\tarrival\truntime\tpriority\n")
        for p in procs: f.write("%d\t%d\t%d\t%d\n" % p)
    with open(os.path.join(d, "resources.txt"), "w") as f:
        f.write("#pid\tresourceid\tcapturetime\treleasetime\n")
        for pid, lst in uses.items():
            for r, c, rl in lst: f.write("%d\t%s\t%d\t%d\n" % (pid, r, c, rl))

work = os.path.join(HERE, "run", "random")
fails = 0
for k in range(N):
    procs, uses = gen_case()
    algo = rnd.choice([1, 2, 3, 3, 4, 4])
    quantum = rnd.randint(1, 5) if algo == 2 else 0
    cs = rnd.choice([0, 1, 1, 1, 2]) if algo == 2 else 1
    shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    for b in ("process_generator.out", "scheduler.out", "process.out"):
        shutil.copy(os.path.join(os.environ.get("BIN_DIR", ROOT), b), work)
    shutil.copy(os.path.join(HERE, "fast_clk.out"), os.path.join(work, "clk.out"))
    write_case(work, procs, uses)
    args = [str(algo)] + ([str(quantum), str(cs)] if algo == 2 else [])
    env = dict(os.environ, TICK_US=TICK)
    try:
        r = subprocess.run(["./process_generator.out"] + args, cwd=work, env=env, capture_output=True, timeout=300)
    except subprocess.TimeoutExpired:
        print("TIMEOUT case", k); fails += 1; continue
    exp_log, exp_perf = simulate(procs, uses, algo, quantum, cs, preempt_at_dispatch=os.environ.get('PAD', '0') != '0')
    try:
        got_log = [l.rstrip("\n") for l in open(os.path.join(work, "scheduler.log")) if not l.startswith("#")]
        got_perf = [l.rstrip("\n") for l in open(os.path.join(work, "scheduler.perf"))]
    except FileNotFoundError:
        got_log, got_perf = None, None
    if got_log != exp_log or got_perf != exp_perf or r.returncode != 0:
        fails += 1
        keep = os.path.join(HERE, "run", "fail_%d" % k)
        shutil.rmtree(keep, ignore_errors=True); shutil.copytree(work, keep)
        open(os.path.join(keep, "args"), "w").write(" ".join(args))
        print("MISMATCH case %d algo %d args %s (saved in %s) rc=%s" % (k, algo, args, keep, r.returncode))
print("%d/%d cases matched" % (N - fails, N))
sys.exit(1 if fails else 0)
