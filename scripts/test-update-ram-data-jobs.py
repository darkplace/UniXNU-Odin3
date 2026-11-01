#!/usr/bin/env python3
"""Host test of update-ram-data-jobs.py on plain directories (no macOS, no image, no device)."""
import os
from pathlib import Path
import plistlib
import runpy
import tempfile
import unittest

tool = runpy.run_path(str(Path(__file__).with_name('update-ram-data-jobs.py')))
recipe = runpy.run_path(str(Path(__file__).with_name('prepare-ram-data.py')))
FAIRPLAY = 'com.apple.fairplayd.A2'


def os_plist(label):
    spec = tool['KNOWN_JOBS'][label]
    job = dict(Label=label)
    if spec['program']:
        job['Program'] = spec['program']
    if spec['arguments']:
        job['ProgramArguments'] = spec['arguments']
    return plistlib.dumps(job)


class JobsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.daemons = self.root / 'LaunchDaemons'
        self.daemons.mkdir()
        for label, arguments in recipe['DISABLED_JOBS'].items():
            (self.daemons / (label + '.plist')).write_bytes(plistlib.dumps(
                dict(Label=label, ProgramArguments=arguments, _PanicOnCrash=dict(PanicOnConsecutiveCrash=True))))
        for label in tool['KNOWN_JOBS']:
            (self.daemons / (label + '.plist')).write_bytes(os_plist(label))
        self.data = self.root / 'data'
        self.data.mkdir()
        # the exact policy the recipe writes
        recipe['prepare_launch_policy'](self.data, self.daemons)
        self.policy = self.data / tool['POLICY']

    def test_adds_one_job_in_place_and_keeps_everything_else(self):
        (self.data / 'mobile').mkdir()
        (self.data / 'mobile/keep').write_bytes(b'x' * 100)
        before = tool['inventory'](self.data)
        inode, original = self.policy.stat().st_ino, self.policy.read_bytes()
        result = tool['merge_policy'](self.data, self.daemons, [FAIRPLAY])
        self.assertEqual(result, sorted([*recipe['DISABLED_JOBS'], FAIRPLAY]))
        self.assertEqual(self.policy.stat().st_ino, inode)
        self.assertEqual(self.policy.stat().st_mode & 0o777, 0o644)
        self.assertEqual(plistlib.loads(self.policy.read_bytes()), dict.fromkeys(result, True))
        self.assertEqual(self.policy.read_bytes(), plistlib.dumps(dict.fromkeys(result, True)))   # recipe format, sorted keys
        self.assertGreater(len(self.policy.read_bytes()), len(original))
        self.assertLess(len(self.policy.read_bytes()), 4096)             # still one 4 KiB block
        changed, ignored = tool['differences'](before, tool['inventory'](self.data))
        self.assertEqual((changed, ignored), ([tool['POLICY']], []))

    def test_difference_report_catches_other_changes_and_ignores_macos_housekeeping(self):
        (self.data / 'mobile').mkdir()
        (self.data / 'mobile/keep').write_bytes(b'x')
        before = tool['inventory'](self.data)
        (self.data / 'mobile/keep').write_bytes(b'y')
        (self.data / '.fseventsd').mkdir()
        (self.data / '.fseventsd/uuid').write_bytes(b'z')
        (self.data / 'new').write_bytes(b'')
        os.chmod(self.data / 'mobile', 0o700)
        changed, ignored = tool['differences'](before, tool['inventory'](self.data))
        self.assertEqual(changed, ['mobile', 'mobile/keep', 'new'])
        self.assertEqual(ignored, ['.fseventsd', '.fseventsd/uuid'])

    def test_refusals_leave_the_policy_untouched(self):
        original = self.policy.read_bytes()
        merge = tool['merge_policy']
        for labels, message in (([FAIRPLAY, FAIRPLAY], 'once'), ([], 'once'),
                                (['com.apple.watchdogd'], 'qualified'), (['com.apple.nothing'], 'qualified')):
            with self.assertRaisesRegex(ValueError, message):
                merge(self.data, self.daemons, labels)
        self.assertEqual(self.policy.read_bytes(), original)
        # the OS volume's plist must be what KNOWN_JOBS was written from
        (self.daemons / (FAIRPLAY + '.plist')).write_bytes(plistlib.dumps(dict(Label=FAIRPLAY, Program='/usr/sbin/other')))
        with self.assertRaisesRegex(ValueError, 'Program'):
            merge(self.data, self.daemons, [FAIRPLAY])
        (self.daemons / (FAIRPLAY + '.plist')).write_bytes(plistlib.dumps(dict(Label='x', Program='/usr/sbin/fairplayd.A2')))
        with self.assertRaisesRegex(ValueError, 'Label'):
            merge(self.data, self.daemons, [FAIRPLAY])
        (self.daemons / (FAIRPLAY + '.plist')).write_bytes(os_plist(FAIRPLAY))
        # a policy that is not exactly the recipe's (missing job, extra job) is refused
        self.policy.write_bytes(plistlib.dumps(dict.fromkeys(list(recipe['DISABLED_JOBS'])[1:], True)))
        with self.assertRaisesRegex(ValueError, 'differs'):
            merge(self.data, self.daemons, [FAIRPLAY])
        self.policy.write_bytes(plistlib.dumps(dict.fromkeys([*recipe['DISABLED_JOBS'], 'extra'], True)))
        with self.assertRaisesRegex(ValueError, 'differs'):
            merge(self.data, self.daemons, [FAIRPLAY])

    def test_second_step_needs_the_first_as_existing_state(self):
        merge = tool['merge_policy']
        step1 = merge(self.data, self.daemons, [FAIRPLAY])
        with self.assertRaisesRegex(ValueError, 'differs'):
            merge(self.data, self.daemons, ['com.apple.coreduetd'])
        step2 = merge(self.data, self.daemons, ['com.apple.coreduetd'], existing=step1)
        self.assertEqual(step2, sorted([*step1, 'com.apple.coreduetd']))
        with self.assertRaisesRegex(ValueError, 'Already'):
            merge(self.data, self.daemons, ['com.apple.coreduetd'], existing=step2)

    def test_symlinks_and_a_nonempty_marker_are_refused(self):
        marker = self.data / tool['MARKER']
        marker.write_bytes(b'x')
        with self.assertRaisesRegex(ValueError, 'recipe'):
            tool['merge_policy'](self.data, self.daemons, [FAIRPLAY])
        marker.write_bytes(b'')
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'com.apple.xpc.launchd').symlink_to(self.data / 'db/com.apple.xpc.launchd', target_is_directory=True)
        (self.data / 'db/com.apple.xpc.launchd').rename(self.data / 'db/real')
        (self.data / 'db/com.apple.xpc.launchd').symlink_to(self.data / 'db/real', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            tool['merge_policy'](self.data, self.daemons, [FAIRPLAY])

    def test_every_known_job_is_a_real_label_and_has_one_command_form(self):
        for label, spec in tool['KNOWN_JOBS'].items():
            self.assertTrue(label.startswith('com.apple.'))
            self.assertEqual((spec['program'] is None), (spec['arguments'] is not None), label)
            self.assertNotIn(label, recipe['DISABLED_JOBS'])


if __name__ == '__main__':
    unittest.main()
