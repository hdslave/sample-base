# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise startup cleanup with an isolated environment, never real flash."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = '473fb7260e597e61cc55d9d30595eb9d9aa1304ff1b88960e633ad19457dc91a'
MOCK = '''import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
state = pathlib.Path(os.environ['TEST_ENV'])
if name == 'uname':
    print(os.environ.get('MACHINE', 'aarch64'))
elif name == 'sha256sum':
    print(os.environ['FIRMWARE'] + '  /usr/bin/MPC Sample')
elif name == 'fw_printenv':
    if os.environ.get('READ_FAIL'): sys.exit(1)
    for key, value in json.loads(state.read_text()).items(): print(key + '=' + value)
elif name == 'fw_setenv':
    assert sys.argv[1:] == ['imb-dev-mode-used'], sys.argv
    with open(os.environ['TEST_WRITES'], 'a') as log: log.write('write\\n')
    if os.environ.get('WRITE_FAIL'): sys.exit(1)
    values = json.loads(state.read_text())
    if not os.environ.get('KEEP_MARKER'): values.pop('imb-dev-mode-used', None)
    state.write_text(json.dumps(values))
'''


class MarkerTests(unittest.TestCase):
    def run_cleanup(self, values, **options):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            state, writes = temp / 'state', temp / 'writes'
            state.write_text(json.dumps(values))
            for name in ('uname', 'sha256sum', 'fw_printenv', 'fw_setenv'):
                tool = temp / name
                tool.write_text('#!' + sys.executable + '\n' + MOCK)
                tool.chmod(0o755)
            env = {**os.environ, 'PATH': str(temp) + os.pathsep + os.environ['PATH'],
                   'TEST_ENV': str(state), 'TEST_WRITES': str(writes), 'FIRMWARE': FIRMWARE, **options}
            result = subprocess.run(['/bin/sh', str(ROOT / 'install/clear-dev-marker.sh')],
                                    capture_output=True, text=True, env=env, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(state.read_text()), writes.read_text() if writes.exists() else '', result

    def test_removes_only_exact_marker_and_is_idempotent(self):
        other = {'serial#': 'example', 'imb-beta-features-used': '1', 'imb-dev-mode-used-extra': '1'}
        for value in ('1', ''):
            values, writes, result = self.run_cleanup({**other, 'imb-dev-mode-used': value})
            self.assertEqual(values, other)
            self.assertEqual(writes, 'write\n')
            self.assertIn('cleared imb-dev-mode-used', result.stdout)
            self.assertEqual(self.run_cleanup(values)[1], '')

    def test_skips_1_3_unknown_firmware_and_other_machines(self):
        values = {'imb-dev-mode-used': '1'}
        for options in ({'FIRMWARE': 'b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508'},
                        {'FIRMWARE': '0' * 64}, {'MACHINE': 'x86_64'}):
            after, writes, _ = self.run_cleanup(values, **options)
            self.assertEqual((after, writes), (values, ''))

    def test_both_supported_1_4_firmwares(self):
        values, writes, _ = self.run_cleanup({'imb-dev-mode-used': '1'},
            FIRMWARE='2271252ffded26830546d4ee1852e39a2c1f4c574a40c47fba10b5a397727ec2')
        self.assertEqual((values, writes), ({}, 'write\n'))

    def test_unreadable_environment_is_not_written(self):
        values = {'imb-dev-mode-used': '1'}
        after, writes, result = self.run_cleanup(values, READ_FAIL='1')
        self.assertEqual((after, writes), (values, ''))
        self.assertIn('cannot read environment', result.stderr)

    def test_failed_or_ineffective_write_does_not_block_startup(self):
        values = {'imb-dev-mode-used': '1'}
        for option, warning in (('WRITE_FAIL', 'could not clear marker'), ('KEEP_MARKER', 'marker still present')):
            after, writes, result = self.run_cleanup(values, **{option: '1'})
            self.assertEqual((after, writes), (values, 'write\n'))
            self.assertIn(warning, result.stderr)

    def test_sd_and_usb_start_cleanup_after_stock_stops(self):
        from device import service_unit
        for text in (service_unit('/run/sample-base-release-abc'), (ROOT / 'install/sd-worker.sh').read_text(),
                     (ROOT / 'install/sd-update.sh').read_text()):
            self.assertLess(text.index('ExecStartPre=/bin/systemctl stop ac50.service'),
                            text.index('ExecStartPre=-/bin/sh '))
            self.assertLess(text.index('/clear-dev-marker.sh\n'), text.index('ExecStart='))


if __name__ == '__main__':
    unittest.main()
