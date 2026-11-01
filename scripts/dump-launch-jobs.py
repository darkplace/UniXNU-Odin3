#!/usr/bin/env python3
"""Read the launchd job plists of an attached OS volume (read only) and print what decides whether a job loops.

  python3 dump-launch-jobs.py ~/odin3/mnt/os/System/Library/LaunchDaemons --names fairplayd.A2,passd,coreduetd
  python3 dump-launch-jobs.py ~/odin3/mnt/os/System/Library/LaunchDaemons --keepalive [--seed-disabled label,label]

--names     jobs whose file name contains any of the comma-separated fragments: Label, Program/ProgramArguments, KeepAlive,
            RunAtLoad, ThrottleInterval, UserName, mach services and launch events.
--keepalive the jobs with an unconditional KeepAlive=true (the ones launchd restarts every time they exit).
Plain `plistlib`: it runs on macOS' python3 3.9 and on Linux. It reads configuration, never code.
"""
import argparse
import glob
import os
from pathlib import Path
import plistlib

KEYS = ("Program", "ProgramArguments", "KeepAlive", "RunAtLoad", "ThrottleInterval", "UserName",
        "EnablePressuredExit", "ProcessType", "_PanicOnCrash", "ExitTimeOut", "StartInterval")


def jobs(folder):
    for path in sorted(glob.glob(os.path.join(str(folder), "*.plist"))):
        yield Path(path).name, plistlib.loads(Path(path).read_bytes())


def describe(job):
    fields = {key: job[key] for key in KEYS if key in job}
    mach, events = list(job.get("MachServices", {})), list(job.get("LaunchEvents", {}))
    more = "+%d" % (len(mach) - 4) if len(mach) > 4 else ""
    return "%s :: %s :: mach=%s%s :: events=%s" % (job.get("Label"), fields, mach[:4], more, events)


def keepalive(folder, disabled):
    rows = [job for _, job in jobs(folder) if job.get("KeepAlive") is True]
    lines = ["KeepAlive=true (unconditional) jobs: %d of %d plists" % (len(rows), len(list(jobs(folder))))]
    for job in rows:
        program = job.get("Program") or (job.get("ProgramArguments") or ["?"])[0]
        lines.append("  label=%s program=%s RunAtLoad=%s Throttle=%s disabled_in_seed=%s" % (
            job.get("Label"), program, job.get("RunAtLoad"), job.get("ThrottleInterval"), job.get("Label") in disabled))
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--names", help="comma-separated file-name fragments")
    parser.add_argument("--keepalive", action="store_true")
    parser.add_argument("--seed-disabled", default="", help="comma-separated labels the seed already disables")
    args = parser.parse_args()
    if bool(args.names) == args.keepalive:
        parser.error("give exactly one of --names and --keepalive")
    if args.keepalive:
        print("\n".join(keepalive(args.folder, set(filter(None, args.seed_disabled.split(","))))))
        return
    wanted = args.names.split(",")
    for name, job in jobs(args.folder):
        if any(fragment in name for fragment in wanted):
            print(describe(job))


if __name__ == "__main__":
    main()
