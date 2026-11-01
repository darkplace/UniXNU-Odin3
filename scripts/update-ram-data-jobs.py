#!/usr/bin/env python3
"""Disable more launchd jobs in an already prepared RAM Data seed, keeping its volume identity.

Runs on macOS (hdiutil, diskutil) as an ordinary user: no root. The seed from prepare-ram-data.py is copied, the copy is
edited in place and the original is never opened for writing. The volume UUID, and with it the kernel (it compiles the UUID
in), stay as they are. Only /private/var/db/com.apple.xpc.launchd/disabled.plist grows.

Why no root is enough, and how it is proven: attached without -owners on, a user may write the files of the volume and an
in-place write keeps the stored uid/gid/mode of an existing inode. Attached read-only WITH -owners on, the real stored owners
show even to a user. The tool lists every entry (type, uid, gid, mode, size, SHA-256 or link target) before and after, and
fails unless the only difference is disabled.plist and its owner and mode are the same.

  python3 update-ram-data-jobs.py --tree ~/odin3/tree --os-root ~/odin3/mnt/os \\
      --seed ~/odin3/out/data.cdr --seed-sha256 5d844aef... --seed-encoded-sha256 2e390e74... \\
      --volume-uuid 5D8FE226-65AB-46F4-92F2-C801EE86F627 --output ~/odin3/out-fairplayd --job com.apple.fairplayd.A2

--os-root is the pinned OS volume already attached read-only (prepare-ram-data.py attaches the DMG itself; here the Label and
Program of every job are read from that volume and compared with KNOWN_JOBS, and sbin/launchd must be the pinned one).
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import plistlib
import runpy
import shutil
import stat
import subprocess
import sys

HERE = Path(__file__).resolve().parent
recipe = runpy.run_path(str(HERE / 'prepare-ram-data.py'))
BASE_JOBS = tuple(recipe['DISABLED_JOBS'])
LAUNCHD_SHA256 = '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25'   # same pin as prepare-ram-data.py
POLICY = 'db/com.apple.xpc.launchd/disabled.plist'
MARKER = 'db/com.apple.xpc.launchd/disabled.migrated'
# Entries macOS itself creates on a volume it mounts. They are reported, never an error.
VOLATILE = ('.fseventsd', '.Spotlight-V100', '.Trashes', '.TemporaryItems', '.DocumentRevisions-V100')

# Jobs this tool may add, read from the 21H461 OS volume (044-76040-038.dmg) on the Mac, 2026-10-30.
# 'program' is the launchd `Program` key, 'arguments' the `ProgramArguments`; the OS plist must carry exactly what is written here.
KNOWN_JOBS = {
    'com.apple.fairplayd.A2': dict(program='/usr/sbin/fairplayd.A2', arguments=None,
                                   note='KeepAlive=true, no RunAtLoad; exits at once and is relaunched (22 launches in 6 min on sheng)'),
    'com.apple.passd': dict(program=None, arguments=['/System/Library/PrivateFrameworks/PassKitCore.framework/passd'],
                            note='no KeepAlive; crashes on sheng (int-176)'),
    'com.apple.mobile.obliteration': dict(program=None, arguments=['/usr/libexec/mobile_obliterator'],
                                          note='no KeepAlive, no UserName (root): a data-wipe daemon'),
    'com.apple.coreduetd': dict(program='/usr/libexec/coreduetd', arguments=None,
                                note='no KeepAlive; one thread above 50 % of a core on sheng'),
}


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024**2), b''):
            digest.update(chunk)
    return digest.hexdigest()


def check_original(daemons, label):
    """The OS volume's own plist for `label` must be the one KNOWN_JOBS was written from."""
    if label not in KNOWN_JOBS:
        raise ValueError('Job not qualified for this tool: ' + label)
    expected = KNOWN_JOBS[label]
    job = plistlib.loads((Path(daemons) / (label + '.plist')).read_bytes())
    if job.get('Label') != label:
        raise ValueError('Unexpected original launch policy (Label): ' + label)
    if job.get('Program') != expected['program'] or job.get('ProgramArguments') != expected['arguments']:
        raise ValueError('Unexpected original launch policy (Program/ProgramArguments): ' + label)


def merge_policy(target, daemons, labels, existing=BASE_JOBS):
    """Add `labels` to disabled.plist of the mounted Data volume `target`, in place. Returns the new sorted label list."""
    target = Path(target)
    labels = list(labels)
    if not labels or len(set(labels)) != len(labels):
        raise ValueError('Give each job once')
    for label in labels:
        check_original(daemons, label)
    for relative in ('db', 'db/com.apple.xpc.launchd'):
        if (target / relative).is_symlink():
            raise ValueError('Refuse symlink in launch policy path')
    path, marker = target / POLICY, target / MARKER
    if path.is_symlink() or not path.is_file() or marker.is_symlink() or not marker.is_file() or marker.stat().st_size:
        raise ValueError('Launch policy is not the one the recipe writes')
    current = plistlib.loads(path.read_bytes())
    if current != dict.fromkeys(existing, True):
        raise ValueError('Existing launch policy differs from the expected one: ' + ', '.join(sorted(current)))
    already = [label for label in labels if label in current]
    if already:
        raise ValueError('Already disabled: ' + ', '.join(already))
    updated = dict(current)
    updated.update(dict.fromkeys(labels, True))
    data = plistlib.dumps(updated)          # the recipe's own format: XML, sorted keys
    before = path.lstat()
    with path.open('r+b') as stream:
        stream.write(data)
        stream.truncate(len(data))
        stream.flush()
        os.fsync(stream.fileno())
    if path.read_bytes() != data:
        raise ValueError('Launch policy readback mismatch')
    after = path.lstat()
    if (after.st_ino, after.st_uid, after.st_gid, stat.S_IMODE(after.st_mode)) != (
            before.st_ino, before.st_uid, before.st_gid, stat.S_IMODE(before.st_mode)):
        raise ValueError('Launch policy inode, owner or mode changed')
    return sorted(updated)


def inventory(root):
    """{relative path: (kind, uid, gid, mode, size, sha256 or link target)} for everything under root, without following links."""
    root = Path(root)
    result = {}
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in sorted([*dirs, *files]):
            path = Path(current) / name
            metadata = path.lstat()
            relative = str(path.relative_to(root))
            mode = stat.S_IMODE(metadata.st_mode)
            if stat.S_ISLNK(metadata.st_mode):
                entry = ('link', metadata.st_uid, metadata.st_gid, mode, 0, os.readlink(path))
            elif stat.S_ISDIR(metadata.st_mode):
                entry = ('dir', metadata.st_uid, metadata.st_gid, mode, 0, '')
            elif stat.S_ISREG(metadata.st_mode):
                entry = ('file', metadata.st_uid, metadata.st_gid, mode, metadata.st_size, file_sha256(path))
            else:
                raise ValueError('Unsupported entry: ' + relative)
            result[relative] = entry
    return result


def volatile(relative):
    return relative.split('/')[0] in VOLATILE


def differences(before, after):
    """(changed, ignored): paths whose entry differs, split into real ones and macOS's own housekeeping entries."""
    changed, ignored = [], []
    for relative in sorted(set(before) | set(after)):
        if before.get(relative) != after.get(relative):
            (ignored if volatile(relative) else changed).append(relative)
    return changed, ignored


@contextmanager
def attached(image, mountpoint, readonly, owners):
    """hdiutil attach; yields the volume's /dev entry. -owners on shows (and enforces) the stored owners even to a user."""
    mountpoint = Path(mountpoint)
    mountpoint.mkdir(parents=True, exist_ok=True)
    command = ['hdiutil', 'attach', '-nobrowse', '-owners', 'on' if owners else 'off', '-noautofsck']
    if readonly:
        command.append('-readonly')
    result = plistlib.loads(subprocess.check_output([*command, '-mountpoint', str(mountpoint), '-plist', str(image)]))
    device = result['system-entities'][0]['dev-entry']
    try:
        volumes = [e for e in result['system-entities'] if Path(e.get('mount-point', '/nonexistent')).resolve() == mountpoint.resolve()]
        if len(volumes) != 1:
            raise ValueError('Require exactly one mounted volume')
        yield volumes[0]['dev-entry']
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)


def volume_uuid(device):
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
    return info['VolumeUUID'].upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True, help='folder holding drivers/PaganiDarwinBoot/Test/packed_ramdisk.py')
    parser.add_argument('--os-root', type=Path, required=True, help='the pinned OS volume, already attached read-only')
    parser.add_argument('--seed', type=Path, required=True, help='data.cdr from prepare-ram-data.py (never modified)')
    parser.add_argument('--seed-sha256', required=True, help='SHA-256 of --seed (the "logical sha256" of the recipe)')
    parser.add_argument('--seed-encoded-sha256', required=True, help='SHA-256 of the DBRDISK1 file the recipe packed from --seed')
    parser.add_argument('--volume-uuid', required=True, help='the UUID compiled into the kernel (CONFIG_XNU_OWNER_DATA_VOLUME_UUID)')
    parser.add_argument('--output', type=Path, required=True, help='new folder: data.cdr, data.dbrdisk, seed.json')
    parser.add_argument('--job', action='append', required=True, help='label to disable; one of ' + ', '.join(sorted(KNOWN_JOBS)))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('run as an ordinary user: root would hide the ownership check this tool relies on')
    args.output = args.output.expanduser().resolve()
    seed, os_root, tree = (p.expanduser().resolve() for p in (args.seed, args.os_root, args.tree))
    if args.output.exists():
        parser.error('--output must be a NEW folder')
    unknown = [j for j in args.job if j not in KNOWN_JOBS]
    if unknown:
        parser.error('not qualified: ' + ', '.join(unknown))
    daemons = os_root / 'System/Library/LaunchDaemons'
    if file_sha256(os_root / 'sbin/launchd') != LAUNCHD_SHA256:
        parser.error('--os-root is not the pinned 21H461 OS volume (launchd differs)')
    for label in args.job:
        check_original(daemons, label)
    if file_sha256(seed) != args.seed_sha256:
        parser.error('--seed SHA-256 mismatch')
    pack = runpy.run_path(str(tree / 'drivers/PaganiDarwinBoot/Test/packed_ramdisk.py'))['pack']

    args.output.mkdir(parents=True)
    work = args.output / 'work'
    # Packing must be deterministic: the unmodified seed has to give back the file the medium already carries.
    control = pack(seed, args.output / 'control.dbrdisk')
    if control['encoded_sha256'] != args.seed_encoded_sha256 or control['sha256'] != args.seed_sha256:
        raise SystemExit('packing the unmodified seed does not reproduce the known DBRDISK1: ' + json.dumps(control))
    (args.output / 'control.dbrdisk').unlink()

    image = args.output / 'data.cdr'
    shutil.copyfile(seed, image)
    with attached(image, work / 'before', readonly=True, owners=True) as device:
        if volume_uuid(device) != args.volume_uuid.upper():
            raise SystemExit('volume UUID of the seed is not the one given')
        before = inventory(work / 'before')
    with attached(image, work / 'edit', readonly=False, owners=False):
        disabled = merge_policy(work / 'edit', daemons, args.job)
    with attached(image, work / 'after', readonly=True, owners=True) as device:
        if volume_uuid(device) != args.volume_uuid.upper():
            raise SystemExit('volume UUID changed')
        after = inventory(work / 'after')
        fsck = subprocess.run(['fsck_apfs', '-n', device], capture_output=True, text=True)
    changed, ignored = differences(before, after)
    if changed != [POLICY]:
        raise SystemExit('unexpected difference in the volume: ' + json.dumps(changed))
    if before[POLICY][1:4] != after[POLICY][1:4] or after[POLICY][0] != 'file':
        raise SystemExit('disabled.plist owner or mode changed: %r -> %r' % (before[POLICY], after[POLICY]))
    shutil.rmtree(work)
    with image.open('rb') as stream:
        header = stream.read(4096)
    if image.stat().st_size != 1024**3 or header[32:36] != b'NXSB':
        raise SystemExit('unexpected APFS seed geometry')
    packed = pack(image, args.output / 'data.dbrdisk')
    report = dict(volume_uuid=args.volume_uuid.upper(), source_seed_sha256=args.seed_sha256,
                  source_encoded_sha256=args.seed_encoded_sha256, packed=packed,
                  jobs_added=args.job, disabled_jobs=disabled,
                  policy_before=dict(zip(('kind', 'uid', 'gid', 'mode', 'size', 'sha256'), before[POLICY])),
                  policy_after=dict(zip(('kind', 'uid', 'gid', 'mode', 'size', 'sha256'), after[POLICY])),
                  entries_compared=len(before), entries_changed=changed, macos_housekeeping_changed=ignored,
                  fsck_apfs_n=dict(returncode=fsck.returncode, tail=(fsck.stdout + fsck.stderr).strip().splitlines()[-3:]),
                  ownership_verified_with='hdiutil attach -readonly -owners on, no root', hardware_tested=False)
    (args.output / 'seed.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    sys.exit(main())
