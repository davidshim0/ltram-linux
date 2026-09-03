#!/usr/bin/env python3
"""How many of a process's pages are on LtRAM right now.

    ltram_resident.py <pid>   ->  ltram_pages total_pages anon_pages

Counts PFNs falling inside the LtRAM zone, whose bounds debugfs publishes as
start_pfn/end_pfn. Needs root: pagemap zeroes the PFN field otherwise, and a
zeroed PFN silently reads as "not on LtRAM" rather than as an error.
"""
import os, sys
import numpy as np

PAGE = os.sysconf("SC_PAGE_SIZE")
PM_PRESENT, PFN_MASK = 1 << 63, (1 << 55) - 1
D = "/sys/kernel/debug/ltram"

def bound(name):
    with open(f"{D}/{name}") as f:
        return int(f.read().split()[0])

def main(pid):
    lo, hi = bound("start_pfn"), bound("end_pfn")
    if os.geteuid() != 0:
        sys.exit("must be root: pagemap zeroes PFNs otherwise")
    ltram = total = anon = 0
    regions = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            p = line.split()
            name = p[5] if len(p) > 5 else ""
            if name in ("[vsyscall]", "[vvar]", "[vdso]"):
                continue
            a, b = (int(x, 16) for x in p[0].split("-"))
            regions.append((a, b, name == "" or name.startswith("[")))
    with open(f"/proc/{pid}/pagemap", "rb") as f:
        for a, b, is_anon in regions:
            try:
                f.seek((a // PAGE) * 8)
                raw = f.read(((b - a) // PAGE) * 8)
            except Exception:
                continue
            if len(raw) < 8:
                continue
            e = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
            pres = (e & np.uint64(PM_PRESENT)) != 0
            n = int(pres.sum())
            total += n
            if is_anon:
                anon += n
            pfn = (e[pres] & np.uint64(PFN_MASK)).astype(np.int64)
            ltram += int(((pfn >= lo) & (pfn < hi)).sum())
    print(ltram, total, anon)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: ltram_resident.py <pid>")
    main(int(sys.argv[1]))
