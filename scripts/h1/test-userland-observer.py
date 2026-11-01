#!/usr/bin/env python3
"""Host regressions for Odin's real renderer and actual prepared exit hook.

No device access. All generated fixture files stay below the project tmp/.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--xnu", type=Path, required=True)
args = parser.parse_args()
device = project / "devices/odin3-pagani"
scratch = project / "tmp"
scratch.mkdir(exist_ok=True)
os.environ["TMPDIR"] = str(scratch.resolve())


def run(command):
    subprocess.run([str(p) for p in command], check=True)


exit_source = (args.xnu / "bsd/kern/kern_exit.c").read_text()
begin = exit_source.index("\t/* exec retires a shadow proc")
end = exit_source.index("\n\tif (task)", begin)
hook = exit_source[begin:end]
exec_source = (args.xnu / "bsd/kern/kern_exec.c").read_text()
switch = exec_source[exec_source.index("\nproc_exec_switch_task("):]
switch = switch[:switch.index("\nstatic ")]
assert switch.index("phash_replace_locked") < switch.index("P_REF_SHADOW") < switch.index("initproc = new_proc")
assert "task_terminate_internal(old_task)" in exec_source
assert '"Boolean indicator that launchd has reached main"' in exit_source
assert "if (p == initproc) {\n\t\t/* Always panic for launchd */\n\t\treturn true;" in exit_source
assert "LIVE initproc exit: critical policy" in hook
generic = (args.xnu / "bsd/kern/sys_generic.c").read_text()
assert "proc_getpid(p) == 1 && (fd == 1 || fd == 2)" in generic
assert "budget = 256" in generic and "i < 4 && budget" in generic

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
enum {P_REF_SHADOW=8};
struct reason {unsigned osr_namespace; uint64_t osr_code;};
struct proc {unsigned p_refcount, p_crash_behavior; int pid; const char *p_name; struct reason *p_exit_reason;};
typedef struct proc *proc_t;
static proc_t initproc;
static unsigned shadows,exits,critical,live_stage;
#define os_atomic_load(p, order) (*(p))
static int proc_getpid(proc_t p){return p->pid;}
static void odin_boot_shadow(int pid,const char *name){assert(pid==1 && name);++shadows;}
static void boot_progress_process_stage(int pid,const char *name,const char *stage){
 assert(pid==1 && name && strstr(stage,"LIVE initproc"));++live_stage;
}
static void boot_progress_exit(int pid,const char *name,int rv,unsigned ns,uint64_t code){
 assert(pid>0 && name && rv==0 && ns==0 && code==0);++exits;
}
static void proc_handle_critical_exit(proc_t p,int rv){assert(p && rv==0);++critical;}
static void observed_prepareexit(proc_t p,int rv){
''' + hook + r'''
}
int main(void){
 struct proc live={0,0,1,"launchd",0},old={P_REF_SHADOW,0,1,"init",0},daemon={0,1,42,"service",0};
 initproc=&live;
 observed_prepareexit(&old,0);assert(shadows==1 && !exits && !critical && !live_stage);
 observed_prepareexit(&live,0);assert(shadows==1 && exits==1 && critical==1 && live_stage==1);
 observed_prepareexit(&daemon,0);assert(exits==2 && critical==2 && live_stage==1);
}
'''

with tempfile.TemporaryDirectory(prefix="observer-", dir=scratch) as temporary:
    folder = Path(temporary)
    source = folder / "exit.c"
    source.write_text(fixture)
    binary = folder / "exit"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g"]
    run(["cc", *flags, source, "-o", binary])
    run([binary])
    console = device / "modules/Pagani/drivers/BootProgress/Test/console.c"
    font = device / "modules/Pagani/drivers/PaganiDarwinBoot/DarwinBootPkg/Driver"
    run(["cc", *flags, "-I", font, console, "-o", folder / "console"])
    run([folder / "console"])
print("PASS: actual exec-shadow/live-init exit classification, unchanged critical policy, bounded log capture and observer renderer")
