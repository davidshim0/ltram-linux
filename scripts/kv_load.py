#!/usr/bin/env python3
"""A YCSB-shaped load for memcached and redis.

YCSB-C has no memcached binding at all, and its redis binding hangs here --
2 keys inserted in 45 s, on its own shipped spec file. Both protocols are
small enough that a client is cheaper to write than to debug, and driving
both servers from ONE generator is better methodology anyway: identical key
distribution, value size and read ratio, so the two censuses differ by the
server and nothing else.

Zipfian keys, a settable read ratio, and it runs until killed -- the census
decides when the run ends, not the load generator.

    ./kv_load.py --proto redis     --keys 700000 --value 1400 --threads 8
    ./kv_load.py --proto memcached --keys 700000 --value 1400 --threads 8
"""
import argparse, socket, threading, random, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--proto", choices=("memcached", "redis"), default="memcached")
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=0)
ap.add_argument("--keys", type=int, default=700_000)
ap.add_argument("--value", type=int, default=1400)
ap.add_argument("--read-ratio", type=float, default=0.95)
ap.add_argument("--threads", type=int, default=8)
ap.add_argument("--zipf", type=float, default=0.99)
ap.add_argument("--ops-per-sec", type=float, default=0,
                help="total offered rate across threads; 0 = unthrottled. "
                     "Unthrottled rewrites the whole keyspace in minutes, "
                     "which is not how a cache behaves.")
ap.add_argument("--load-only", action="store_true")
ap.add_argument("--no-load", action="store_true",
                help="skip the load phase; drive traffic at an already-populated server")
a = ap.parse_args()

if not a.port: a.port = 6379 if a.proto == "redis" else 11211
VAL = ("x" * a.value).encode()
stop = threading.Event()

def conn():
    s = socket.create_connection((a.host, a.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s, s.makefile("rb")

def mc_set(s, f, k):
    s.sendall(b"set key%d 0 0 %d\r\n" % (k, len(VAL)) + VAL + b"\r\n")
    return f.readline()

def mc_get(s, f, k):
    s.sendall(b"get key%d\r\n" % k)
    while True:
        line = f.readline()
        if not line or line.startswith(b"END"): return line
        if line.startswith(b"VALUE"):
            n = int(line.split()[3]); f.read(n + 2)

def rd_set(s, f, k):
    key = b"key%d" % k
    s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n"
              % (len(key), key, len(VAL), VAL))
    return f.readline()

def rd_get(s, f, k):
    key = b"key%d" % k
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(key), key))
    line = f.readline()
    if not line: return None
    if line[:1] == b"$":
        n = int(line[1:])
        if n >= 0: f.read(n + 2)
    return line

setk, getk = (rd_set, rd_get) if a.proto == "redis" else (mc_set, mc_get)

# Load phase, sharded across threads. Sequential, not zipfian: every key has to
# exist before the mix phase, or the read ratio is really a miss ratio.
def load(lo, hi):
    s, f = conn()
    for k in range(lo, hi):
        if setk(s, f, k) is None: return
    s.close()

class Zipf:
    """YCSB's ZipfianGenerator: zeta(n, theta) precomputed, then inverted."""
    def __init__(self, n, theta):
        self.n, self.th = n, theta
        self.zetan = sum(1.0 / (i ** theta) for i in range(1, n + 1))
        self.zeta2 = 1.0 + 0.5 ** theta
        self.alpha = 1.0 / (1.0 - theta)
        self.eta = ((1.0 - (2.0 / n) ** (1.0 - theta))
                    / (1.0 - self.zeta2 / self.zetan))
    def next(self, rnd):
        u = rnd.random(); uz = u * self.zetan
        if uz < 1.0: return 0
        if uz < self.zeta2: return 1
        return min(self.n - 1,
                   int(self.n * ((self.eta * u - self.eta + 1.0) ** self.alpha)))

ZIPF = Zipf(a.keys, a.zipf)

def mix(seed):
    rnd = random.Random(seed)
    s, f = conn()
    n = a.keys
    # Per-thread pacing: a shared token bucket would serialise the threads on
    # one lock at exactly the rates we care about.
    gap = (a.threads / a.ops_per_sec) if a.ops_per_sec else 0.0
    nxt = time.time()
    while not stop.is_set():
        if gap:
            nxt += gap
            d = nxt - time.time()
            if d > 0: time.sleep(d)
            elif d < -1.0: nxt = time.time()      # fell behind; do not burst
        k = ZIPF.next(rnd)
        try:
            if rnd.random() < a.read_ratio: getk(s, f, k)
            else: setk(s, f, k)
        except OSError:
            return

t0 = time.time()
per = (a.keys + a.threads - 1) // a.threads
if a.no_load: ts = []
else:
 ts = [threading.Thread(target=load, args=(i * per, min(a.keys, (i + 1) * per)))
       for i in range(a.threads)]
 [t.start() for t in ts]; [t.join() for t in ts]
 print(f"# loaded {a.keys:,} keys x {a.value} B in {time.time()-t0:.0f}s",
       file=sys.stderr, flush=True)
if a.load_only: sys.exit(0)

ts = [threading.Thread(target=mix, args=(i,), daemon=True) for i in range(a.threads)]
[t.start() for t in ts]
try:
    while True: time.sleep(3600)
except KeyboardInterrupt:
    stop.set()
