#!/usr/bin/env python3
"""Drop test with the two conditions the real setup actually has:

  --second-client  a second attached client that reads at full speed
                   (any setup where a terminal and a web/proxy client are both
                   attached to one session)
  --poke           connect/close on the control socket during the burst
                   (what a tool does when it injects input by connecting to the
                   socket once per message)

usage: droptest2.py <dtach> [--second-client] [--poke] [nlines] [readsize]
"""
import os, re, socket, subprocess, sys, tempfile, threading, time

argv = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = set(a for a in sys.argv[1:] if a.startswith("--"))
dtach = os.path.abspath(argv[0])
N = int(argv[1]) if len(argv) > 1 else 20000
READ = int(argv[2]) if len(argv) > 2 else 512

sock = os.path.join(tempfile.mkdtemp(prefix="droptest2-"), "s.sock")
prog = ["sh", "-c", "sleep 1.5; i=1; while [ $i -le %d ]; do echo LINE$i; "
        "i=$((i+1)); done; sleep 30" % N]
sock_path = sock
master = subprocess.Popen([dtach, "-n", sock, *prog])

def cleanup():
    """Reap anything still holding this run's unique socket path. `dtach -n`
    forks a master that reparents to init, so Popen.terminate() alone leaves it
    (and its producer) running."""
    import signal, subprocess as _sp
    try:
        out = _sp.run(["ps", "-eo", "pid=,command="],
                      capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return
    for line in out.splitlines():
        line = line.strip()
        if sock_path not in line:
            continue
        try:
            os.kill(int(line.split(None, 1)[0]), signal.SIGTERM)
        except Exception:
            pass


import atexit
atexit.register(cleanup)

for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.02)

def attach():
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(sock)
    c.sendall(bytes([1, 0]) + b"\x00" * 8)       # MSG_ATTACH
    return c

slow = attach()
fast = attach() if "--second-client" in flags else None

stop = threading.Event()

def drain_fast():
    fast.settimeout(1)
    while not stop.is_set():
        try:
            if not fast.recv(65536):
                return
        except socket.timeout:
            pass

def poke():
    while not stop.is_set():
        try:
            k = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            k.connect(sock)
            k.close()
        except OSError:
            pass
        time.sleep(0.01)

threads = []
if fast:
    threads.append(threading.Thread(target=drain_fast, daemon=True))
if "--poke" in flags:
    threads.append(threading.Thread(target=poke, daemon=True))
for t in threads:
    t.start()

got = bytearray()
slow.settimeout(3)
deadline = time.time() + 90
while time.time() < deadline:
    try:
        chunk = slow.recv(READ)
    except socket.timeout:
        break
    if not chunk:
        break
    got += chunk
    time.sleep(0.05)
    if b"LINE%d\r\n" % N in got:
        break

stop.set()
slow.close()
master.terminate()

raw = bytes(got)
open("/tmp/droptest2-raw.bin", "wb").write(raw)
seen = set(int(m) for m in re.findall(rb"LINE(\d+)\r*\n", raw))
missing = sorted(set(range(1, N + 1)) - seen)
label = " ".join(sorted(flags)) or "(baseline)"
print("%-28s %5d/%d lines, %7d bytes" % (label, len(seen), N, len(raw)))
if missing:
    runs, start, prev = [], missing[0], missing[0]
    for x in missing[1:] + [None]:
        if x != prev + 1:
            runs.append((start, prev))
            start = x
        prev = x if x is not None else prev
    print("   MISSING %d lines in %d gap(s): %s%s" % (
        len(missing), len(runs), ", ".join("%d-%d" % r for r in runs[:5]),
        " ..." if len(runs) > 5 else ""))
    sys.exit(1)
print("   no gaps")
