#!/usr/bin/env python3
"""Summarise the `UL` lines of a card log (scripts/h1/read-sd-log.sh output) and compare two boots.

  ul-summary.py firmware/out/sdlog-noedge-20261030.txt
  ul-summary.py control.txt --vs treatment.txt          # same numbers for both, then the difference

Per 30 s window: processes, exits in the window, the last process that exited (name:pid), card reads, whether the display is the
system's. Totals: how often a given job was the last to exit, its pids, the first window with a system frame. Nothing here
is a measurement of its own: every number is a field of a UL line, and a field a boot did not print is shown as `-`.
"""
import argparse
from pathlib import Path
import re
import sys

FIELDS = ("t", "main", "procs", "exits", "last", "frames", "own", "rd", "wr", "free", "avail", "aks", "sb", "bb",
          "busy", "rdms", "rdn", "dl", "cs4k")


def parse(text):
    rows = []
    for line in text.splitlines():
        if not line.startswith("UL "):
            continue
        row = {}
        for key in FIELDS:
            m = re.search(r"(?:^|\s)" + key + r"=(\S+)", line)
            if m:
                row[key] = m.group(1)
        if "t" not in row:
            continue
        row["t"] = int(row["t"])
        for key in ("procs", "exits", "main", "frames", "own"):
            if key in row:
                row[key] = int(row[key])
        if "rd" in row:
            row["rd"] = int(row["rd"].removesuffix("MiB"))
        name, _, rest = row.get("last", "-:0:0").partition(":")
        row["last_name"], row["last_pid"] = name, int(rest.split(":")[0] or 0)
        rows.append(row)
    rows.sort(key=lambda r: r["t"])
    return rows


def summary(rows, job):
    window = [r for r in rows if r.get("procs", 0) > 3]
    mine = [r for r in window if r["last_name"] and job.startswith(r["last_name"]) and r["last_name"] != "-"]
    frames = next((r["t"] for r in rows if r.get("frames", 0) > 0), None)
    main = next((r["t"] for r in rows if r.get("main", 0) == 1), None)
    return dict(lines=len(rows), last_t=rows[-1]["t"] if rows else None, main_t=main, frames_t=frames,
                samples=len(window), job_last=len(mine), job_pids=[r["last_pid"] for r in mine],
                exits=rows[-1].get("exits") if rows else None, procs=rows[-1].get("procs") if rows else None,
                rd=rows[-1].get("rd") if rows else None, aks=rows[-1].get("aks") if rows else None)


def table(rows):
    out = ["   t  procs  Δexits  last (name:pid)           Δrd MiB  frames own"]
    previous = None
    for r in rows:
        d_exits = r["exits"] - previous["exits"] if previous and "exits" in r and "exits" in previous else None
        d_rd = r["rd"] - previous["rd"] if previous and "rd" in r and "rd" in previous else None
        out.append(f"{r['t']:4d}  {r.get('procs', '-'):>5}  {d_exits if d_exits is not None else '-':>6}  "
                   f"{r['last_name'] + ':' + str(r['last_pid']):<24}  {d_rd if d_rd is not None else '-':>7}  "
                   f"{r.get('frames', '-'):>6} {r.get('own', '-'):>3}")
        previous = r
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--vs", type=Path, help="second log to compare against")
    parser.add_argument("--job", default="fairplayd.A2", help="process name to count as the last to exit (default %(default)s)")
    args = parser.parse_args()
    a = parse(args.log.read_text(errors="replace"))
    if not a:
        sys.exit(f"{args.log}: no UL lines")
    print(f"== {args.log}")
    print(table(a))
    sa = summary(a, args.job)
    print({k: v for k, v in sa.items()})
    if args.vs:
        b = parse(args.vs.read_text(errors="replace"))
        if not b:
            sys.exit(f"{args.vs}: no UL lines")
        print(f"\n== {args.vs}")
        print(table(b))
        sb = summary(b, args.job)
        print({k: v for k, v in sb.items()})
        print(f"\n== {args.job}: last to exit in {sa['job_last']}/{sa['samples']} samples ({args.log.name}) "
              f"vs {sb['job_last']}/{sb['samples']} ({args.vs.name})")
        print(f"   first system frame at UL t: {sa['frames_t']} vs {sb['frames_t']}; processes at last line: {sa['procs']} vs {sb['procs']}; "
              f"exits at last line: {sa['exits']} (t={sa['last_t']}) vs {sb['exits']} (t={sb['last_t']})")


if __name__ == "__main__":
    main()
