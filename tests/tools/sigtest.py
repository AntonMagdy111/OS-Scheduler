import os, signal, subprocess, sys, time
mode = sys.argv[1]
p = subprocess.Popen(["./process_generator.out", "2", "3", "1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
time.sleep(9)
if mode == "group":
    os.killpg(p.pid, signal.SIGINT)      # like Ctrl-C in a terminal
else:
    os.kill(p.pid, signal.SIGINT)        # only the generator is signalled
try:
    p.wait(timeout=10); print("generator exit code", p.returncode)
except subprocess.TimeoutExpired:
    print("generator did NOT exit"); p.kill()
time.sleep(1)
out = subprocess.run(["ps", "-eo", "pid,stat,comm"], capture_output=True, text=True).stdout.splitlines()
left = [l for l in out if any(n in l for n in ("clk.out", "scheduler.out", "process.out", "process_genera"))]
print("leftover processes:", left or "none")
print("IPC objects:", subprocess.run("ipcs | grep -c '^0x'", shell=True, capture_output=True, text=True).stdout.strip())
